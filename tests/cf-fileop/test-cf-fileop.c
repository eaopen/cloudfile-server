/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * Run the shared write-lifecycle case set against the C seam.
 *
 * The same cloudfile-docker/docs/fileop-cases.json drives the Go suite in
 * fileserver/cf_fileop_test.go. If a case fails here it must be fixed in the
 * spec first, then in both implementations -- never in one of them alone.
 *
 * Build and run with ./run.sh; it needs nothing but glib, because cf-fileop.c
 * and cf-path.c are deliberately free of database and session dependencies.
 */

#include <stdio.h>
#include <string.h>

#include "cf-fileop.h"
#include "cf-path.h"
#include "cf-fileop-cases.h"

static int failures = 0;
static int checks = 0;

static void
fail (const char *group, const char *name, const char *fmt, ...)
{
    va_list ap;
    fprintf (stderr, "FAIL [%s] %s: ", group, name);
    va_start (ap, fmt);
    vfprintf (stderr, fmt, ap);
    va_end (ap);
    fprintf (stderr, "\n");
    failures++;
}

static void
check_str (const char *group, const char *name,
           const char *got, const char *expect)
{
    checks++;
    if (g_strcmp0 (got, expect) != 0)
        fail (group, name, "got \"%s\", expected \"%s\"",
              got ? got : "(null)", expect ? expect : "(null)");
}

static void
check_int (const char *group, const char *name, const char *what,
           int got, int expect)
{
    checks++;
    if (got != expect)
        fail (group, name, "%s: got %d, expected %d", what, got, expect);
}

/* ------------------------------------------------------------- normalize */

static void
run_normalize (void)
{
    for (int i = 0; i < CF_N_NORMALIZE_CASES; i++) {
        const NormalizeCase *c = &cf_normalize_cases[i];
        char *got = cf_path_join (c->dir, c->entry);
        check_str ("normalize", c->name, got, c->expect);
        g_free (got);
    }
}

/* --------------------------------------------------------- has_component */

/*
 * The write lifecycle gate's fake provider refuses on a marked path component,
 * so whether phase 2 of that gate passes for the right reason lives here.
 * Substring matching would make one refusal cover paths the test never named,
 * and an empty component matching everything would make it cover all of them --
 * either way the gate would be green while proving something else.
 */
static void
run_components (void)
{
    for (int i = 0; i < CF_N_COMPONENT_CASES; i++) {
        const ComponentCase *c = &cf_component_cases[i];
        check_int ("has_component", c->name, "match",
                   cf_path_has_component (c->path, c->component) ? 1 : 0,
                   c->expect);
    }
}

/* ------------------------------------------------------------ operations */

static void
run_operations (void)
{
    for (int i = 0; i < CF_N_OPERATION_CASES; i++) {
        const OperationCase *c = &cf_operation_cases[i];
        check_int ("operations", c->op, "valid",
                   cf_fileop_op_valid (c->op) ? 1 : 0, c->valid);
        check_int ("operations", c->op, "source",
                   cf_fileop_op_has_source (c->op) ? 1 : 0, c->source);
        check_int ("operations", c->op, "pathless",
                   cf_fileop_op_pathless (c->op) ? 1 : 0, c->pathless);
        check_int ("operations", c->op, "subject_is_root",
                   cf_fileop_op_subject_is_root (c->op) ? 1 : 0,
                   c->subject_is_root);
    }

    /* NULL is not in the case set because JSON cannot express it, and it is
     * exactly what a call site with an uninitialised op would pass. */
    check_int ("operations", "(null)", "valid",
               cf_fileop_op_valid (NULL) ? 1 : 0, 0);
}

/* -------------------------------------------------------------- dispatch */

/* Verdict codes, matching gen-cases.py. */
#define V_ALLOW  0
#define V_REFUSE 1
#define V_NONE   2      /* provider registered without a prepare hook */

static int stub_ran;
static int stub_committed;
static int stub_aborted;

static int stub_allow    (const CfFileOp *fop, GError **error) { stub_ran++; return 0; }
static int stub_refuse   (const CfFileOp *fop, GError **error) {
    stub_ran++;
    g_set_error (error, g_quark_from_string ("seafile"), 600,
                 "Locked by someone else");
    return -1;
}
static void stub_commit  (const CfFileOp *fop) { stub_committed++; }
static void stub_abort   (const CfFileOp *fop) { stub_aborted++; }

static void
register_verdicts (const int *verdicts, int n)
{
    for (int i = 0; i < n; i++) {
        switch (verdicts[i]) {
        case V_ALLOW:
            cf_fileop_register ("stub-allow", stub_allow, NULL, NULL);
            break;
        case V_REFUSE:
            cf_fileop_register ("stub-refuse", stub_refuse, NULL, NULL);
            break;
        case V_NONE:
            cf_fileop_register ("stub-none", NULL, NULL, NULL);
            break;
        }
    }
}

static void
run_dispatch (void)
{
    for (int i = 0; i < CF_N_DISPATCH_CASES; i++) {
        const DispatchCase *c = &cf_dispatch_cases[i];

        cf_fileop_reset ();
        stub_ran = 0;
        register_verdicts (c->verdicts, c->n_verdicts);

        CfFileOp fop = { .op = CF_OP_CREATE_FILE, .repo_id = "r",
                         .dir = "/a", .name = "b.txt", .user = "u" };
        GError *error = NULL;
        int rc = cf_fileop_prepare (&fop, &error);

        check_int ("dispatch", c->name, "allowed", rc == 0 ? 1 : 0,
                   c->expect_allowed);
        check_int ("dispatch", c->name, "providers run", stub_ran,
                   c->expect_ran);

        /* A refusal must always arrive with a reason attached: the message
         * reaches the end user, and a refusal nobody can act on becomes a
         * support ticket with nothing in it. */
        checks++;
        if (rc != 0 && (!error || !error->message))
            fail ("dispatch", c->name, "refused without a GError");

        g_clear_error (&error);
    }

    cf_fileop_reset ();
}

/* ----------------------------------------------------------------- facts */

/* Prepare codes, matching gen-cases.py. */
#define P_ALLOW    0
#define P_REFUSE   1
#define P_INACTIVE 2

/*
 * Models what a repo-op.c entry point does, so the exactly-once accounting is
 * tested rather than merely asserted in prose:
 *
 *   PREPARE -> [commit attempt] * n -> COMMITTED or ABORTED
 *
 * commit_attempts > 1 is the SEAF_ERR_CONCURRENT_UPLOAD retry loop, and the
 * point of the case is that retrying must not multiply the fact.
 */
static void
simulate_entry_point (const FactCase *c)
{
    gboolean cf_prepared = FALSE;
    CfFileOp fop = { .op = CF_OP_CREATE_FILE, .repo_id = "r",
                     .dir = "/a", .name = "b.txt", .user = "u" };
    GError *error = NULL;

    if (cf_fileop_active ()) {
        if (cf_fileop_prepare (&fop, &error) < 0) {
            g_clear_error (&error);
            return;
        }
        cf_prepared = TRUE;
    }

    for (int attempt = 0; attempt < c->commit_attempts; attempt++) {
        gboolean last = (attempt == c->commit_attempts - 1);
        if (!last)
            continue;               /* earlier attempts lost the race */
        if (!c->succeeded)
            break;

        CF_FILEOP_COMMITTED (CF_OP_CREATE_FILE, .repo_id = "r",
                             .dir = "/a", .name = "b.txt", .user = "u");
        cf_prepared = FALSE;
    }

    if (cf_prepared)
        CF_FILEOP_ABORTED (CF_OP_CREATE_FILE, .repo_id = "r",
                           .dir = "/a", .name = "b.txt", .user = "u");
}

static void
run_facts (void)
{
    for (int i = 0; i < CF_N_FACT_CASES; i++) {
        const FactCase *c = &cf_fact_cases[i];

        cf_fileop_reset ();
        stub_ran = stub_committed = stub_aborted = 0;

        switch (c->prepare) {
        case P_ALLOW:
            cf_fileop_register ("stub", stub_allow, stub_commit, stub_abort);
            break;
        case P_REFUSE:
            cf_fileop_register ("stub", stub_refuse, stub_commit, stub_abort);
            break;
        case P_INACTIVE:
            break;              /* no provider at all */
        }

        simulate_entry_point (c);

        check_int ("facts", c->name, "COMMITTED", stub_committed,
                   c->expect_committed);
        check_int ("facts", c->name, "ABORTED", stub_aborted,
                   c->expect_aborted);
    }

    cf_fileop_reset ();
}

/* -------------------------------------------------------------- baseline */

static int inactive_calls;

static int
counting_prepare (const CfFileOp *fop, GError **error)
{
    inactive_calls++;
    return 0;
}

/*
 * The iron law, asserted rather than assumed: with nothing registered, the
 * seam does not reach a provider, does not allocate a context and does not
 * change a return code.
 */
static void
run_baseline (void)
{
    cf_fileop_reset ();
    inactive_calls = 0;

    check_int ("baseline", "no provider", "active",
               cf_fileop_active () ? 1 : 0, 0);

    CfFileOp fop = { .op = CF_OP_CREATE_FILE, .repo_id = "r",
                     .dir = "/a", .name = "b.txt", .user = "u" };
    GError *error = NULL;
    check_int ("baseline", "no provider", "prepare",
               cf_fileop_prepare (&fop, &error), 0);
    checks++;
    if (error)
        fail ("baseline", "no provider", "prepare set an error");
    g_clear_error (&error);

    /* The macros must not even evaluate their arguments when inactive. */
    check_int ("baseline", "no provider", "macro prepare",
               CF_FILEOP_PREPARE (CF_OP_CREATE_FILE, &error,
                                  .repo_id = "r", .dir = "/a",
                                  .name = "b.txt", .user = "u"), 0);
    CF_FILEOP_COMMITTED (CF_OP_CREATE_FILE, .repo_id = "r", .dir = "/a");
    CF_FILEOP_ABORTED (CF_OP_CREATE_FILE, .repo_id = "r", .dir = "/a");
    check_int ("baseline", "no provider", "provider reached",
               inactive_calls, 0);

    /* And with one registered, the macro does reach it -- otherwise the check
     * above would pass on a seam that is wired to nothing. */
    cf_fileop_register ("counting", counting_prepare, NULL, NULL);
    CF_FILEOP_PREPARE (CF_OP_CREATE_FILE, &error,
                       .repo_id = "r", .dir = "/a", .name = "b.txt",
                       .user = "u");
    check_int ("baseline", "with provider", "provider reached",
               inactive_calls, 1);

    cf_fileop_reset ();
}

/* --------------------------------------------------------------- subject */

static void
run_subjects (void)
{
    /* Batch paths: one entry per name, joined against the same dir. */
    GList *names = NULL;
    names = g_list_append (names, (gpointer)"one.txt");
    names = g_list_append (names, (gpointer)"two.txt");

    CfFileOp batch = { .op = CF_OP_DELETE, .repo_id = "r",
                       .dir = "/a", .names = names, .user = "u" };
    GList *paths = cf_fileop_subject_paths (&batch);
    check_int ("subject", "batch", "count", g_list_length (paths), 2);
    check_str ("subject", "batch first", g_list_nth_data (paths, 0), "/a/one.txt");
    check_str ("subject", "batch second", g_list_nth_data (paths, 1), "/a/two.txt");
    g_list_free_full (paths, g_free);
    g_list_free (names);

    /* Single object: one path, from dir + name. */
    CfFileOp single = { .op = CF_OP_UPDATE_FILE, .repo_id = "r",
                        .dir = "/a", .name = "b.txt", .user = "u" };
    char *one = cf_fileop_subject_path (&single);
    check_str ("subject", "single", one, "/a/b.txt");
    g_free (one);

    /* Pathless ops have no subject at all -- a lock provider keys on this. */
    CfFileOp blocks = { .op = CF_OP_UPLOAD_BLOCKS, .repo_id = "r", .user = "u" };
    checks++;
    if (cf_fileop_subject_path (&blocks) != NULL)
        fail ("subject", "upload-blocks", "expected no subject path");
    checks++;
    if (cf_fileop_subject_paths (&blocks) != NULL)
        fail ("subject", "upload-blocks", "expected no subject paths");

    /* Whole-library ops answer "/" regardless of what dir happens to hold. */
    CfFileOp whole = { .op = CF_OP_SYNC_UPDATE, .repo_id = "r",
                       .dir = "/ignored", .user = "u" };
    char *root = cf_fileop_subject_path (&whole);
    check_str ("subject", "sync-update", root, "/");
    g_free (root);

    /* Source paths, for the ops that write both ends. */
    GList *src_names = NULL;
    src_names = g_list_append (src_names, (gpointer)"x.txt");
    CfFileOp mv = { .op = CF_OP_MOVE, .repo_id = "r", .dir = "/dst",
                    .src_repo_id = "r", .src_dir = "/src",
                    .src_names = src_names, .user = "u" };
    GList *src = cf_fileop_source_paths (&mv);
    check_int ("subject", "move source", "count", g_list_length (src), 1);
    check_str ("subject", "move source", g_list_nth_data (src, 0), "/src/x.txt");
    g_list_free_full (src, g_free);
    g_list_free (src_names);

    /* An op with no source must not invent one. */
    checks++;
    if (cf_fileop_source_paths (&single) != NULL)
        fail ("subject", "update-file", "expected no source paths");
}

/* ------------------------------------------------------------------ main */

int
main (void)
{
    run_normalize ();
    run_components ();
    run_operations ();
    run_dispatch ();
    run_facts ();
    run_baseline ();
    run_subjects ();

    printf ("cf-fileop: %d checks, %d failures\n", checks, failures);

    return failures == 0 ? 0 : 1;
}
