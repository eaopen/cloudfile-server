/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

#include "log.h"
#include "seafile-error.h"
#include "cf-ext.h"
#include "cf-path.h"
#include "cf-fileop.h"
#include "cf-fileop-test.h"

/* Deliberately no seafile-session.h: everything this needs about the running
 * server comes through cf_ext_config_*, so the file stays compilable on its
 * own -- same property as cf-fileop.c and cf-path.c, and the reason the seam
 * can be tested without the seafile build. */

static char *refuse_token;
static char *journal_path;

/* The journal is appended to from every write path, so it needs its own lock:
 * seaf-server handles requests on a thread pool, and interleaved partial lines
 * would make the matrix's counts meaningless in a way that looks like a
 * seam bug rather than a test bug. */
static GMutex journal_lock;

static const char *
phase_name (CfFileOpPhase phase)
{
    switch (phase) {
    case CF_FILEOP_PHASE_PREPARE:   return "PREPARE";
    case CF_FILEOP_PHASE_COMMITTED: return "COMMITTED";
    case CF_FILEOP_PHASE_ABORTED:   return "ABORTED";
    }
    return "UNKNOWN";
}

/* The matching rule itself lives in cf-path.c, so it is covered by the shared
 * case set rather than being private to this file. Whether phase 2 of the gate
 * passes for the right reason depends on it: substring matching would make the
 * refusal cover paths the test never named. */
static gboolean
any_marked (GList *paths)
{
    GList *ptr;
    for (ptr = paths; ptr; ptr = ptr->next) {
        if (cf_path_has_component (ptr->data, refuse_token))
            return TRUE;
    }
    return FALSE;
}

static char *
join_paths (GList *paths)
{
    if (!paths)
        return g_strdup ("-");

    GString *buf = g_string_new ("");
    GList *ptr;

    for (ptr = paths; ptr; ptr = ptr->next) {
        if (buf->len)
            g_string_append_c (buf, ',');
        g_string_append (buf, (char *)ptr->data);
    }

    return g_string_free (buf, FALSE);
}

/*
 * One line per event, space separated, fields in a fixed order so the matrix
 * can parse it without a JSON dependency inside the container:
 *
 *   <phase> <op> <repo_id> <subject paths> <source paths> <user> <commit_id>
 *
 * Every field is present on every line; "-" stands for absent. A format where
 * fields disappear when empty would let a missing field read as a shifted one.
 */
static void
journal (const CfFileOp *fop)
{
    if (!journal_path)
        return;

    GList *subjects = cf_fileop_subject_paths (fop);
    GList *sources = cf_fileop_source_paths (fop);
    char *subject_str = join_paths (subjects);
    char *source_str = join_paths (sources);

    g_mutex_lock (&journal_lock);

    FILE *fp = g_fopen (journal_path, "a");
    if (fp) {
        fprintf (fp, "%s %s %s %s %s %s %s\n",
                 phase_name (fop->phase),
                 fop->op ? fop->op : "-",
                 fop->repo_id ? fop->repo_id : "-",
                 subject_str,
                 source_str,
                 fop->user ? fop->user : "-",
                 (fop->commit_id && *fop->commit_id) ? fop->commit_id : "-");
        fclose (fp);
    } else {
        /* Warn rather than fail the write: this provider must not be able to
         * turn a full disk into a service outage, even in a test deployment. */
        seaf_warning ("CloudFile fileop test: cannot append to %s\n",
                      journal_path);
    }

    g_mutex_unlock (&journal_lock);

    g_free (subject_str);
    g_free (source_str);
    g_list_free_full (subjects, g_free);
    g_list_free_full (sources, g_free);
}

static int
test_prepare (const CfFileOp *fop, GError **error)
{
    journal (fop);

    /* Pathless operations have no subject, so there is nothing for a token to
     * match. Refusing them would be refusing at random -- exactly what the
     * contract tells a lock provider not to do -- and it would also break
     * every chunked upload in the matrix before it reached the entry point
     * actually under test. */
    if (cf_fileop_op_pathless (fop->op))
        return 0;

    GList *subjects = cf_fileop_subject_paths (fop);
    GList *sources = cf_fileop_source_paths (fop);
    gboolean refuse = any_marked (subjects) || any_marked (sources);
    char *subject_str = refuse ? join_paths (subjects) : NULL;

    g_list_free_full (subjects, g_free);
    g_list_free_full (sources, g_free);

    if (!refuse)
        return 0;

    /* CF_ERR_FILE_LOCKED so the matrix can check the whole chain, including
     * that Go maps it to 423 and that Seahub does not flatten it to a 500. */
    g_set_error (error, SEAFILE_DOMAIN, CF_ERR_FILE_LOCKED,
                 "CloudFile fileop test provider refused %s on %s",
                 fop->op, subject_str);
    g_free (subject_str);

    return -1;
}

static void
test_committed (const CfFileOp *fop)
{
    journal (fop);
}

static void
test_aborted (const CfFileOp *fop)
{
    journal (fop);
}

void
cf_fileop_test_init (void)
{
    if (!cf_ext_config_bool ("fileop_test_provider_enabled"))
        return;

    refuse_token = cf_ext_config_string ("fileop_test_refuse_token");
    if (refuse_token && !*refuse_token) {
        g_free (refuse_token);
        refuse_token = NULL;
    }

    journal_path = cf_ext_config_string ("fileop_test_journal");
    if (journal_path && !*journal_path) {
        g_free (journal_path);
        journal_path = NULL;
    }

    g_mutex_init (&journal_lock);

    cf_fileop_register ("fileop-test", test_prepare, test_committed,
                        test_aborted);

    /* Loud on purpose. This provider can refuse writes and it appends to a
     * file on every one of them; it exists to gate the seam and has no place
     * in a production deployment. */
    seaf_warning ("CloudFile: fileop TEST provider is enabled "
                  "(refuse_token=%s, journal=%s). "
                  "This is for the write lifecycle gate only -- "
                  "do not run it in production.\n",
                  refuse_token ? refuse_token : "(none)",
                  journal_path ? journal_path : "(none)");
}
