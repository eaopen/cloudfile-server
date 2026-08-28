/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * Run the shared ACL case set against the C resolver.
 *
 * The same cloudfile-docker/docs/acl-cases.json drives the Python suite in
 * cloudfile-hub and the Go suite in fileserver/share. If a case fails here it
 * must be fixed in the spec first, then in all three implementations -- never
 * in one of them alone.
 *
 * Build and run with ./run.sh; it needs nothing but glib.
 */

#include <stdio.h>
#include <string.h>

#include "cf-acl-resolve.h"
#include "cf-acl-cases.h"

static int failures = 0;
static int checks = 0;

static GHashTable *
build_subjects (const Case *tcase)
{
    GHashTable *subjects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);
    g_hash_table_add (subjects,
                      cf_acl_subject_key (CF_SUBJ_USER, tcase->user));

    const char **p;
    for (p = tcase->groups; *p; p++)
        g_hash_table_add (subjects, cf_acl_subject_key (CF_SUBJ_GROUP, *p));
    for (p = tcase->depts; *p; p++)
        g_hash_table_add (subjects, cf_acl_subject_key (CF_SUBJ_DEPT, *p));

    return subjects;
}

static GList *
build_rules (const Case *tcase)
{
    GList *rules = NULL;
    int i;

    for (i = 0; i < tcase->n_rules; i++) {
        const CaseRule *r = &tcase->rules[i];
        rules = g_list_prepend (rules, cf_acl_rule_new (
            r->path,
            cf_acl_subject_type_to_level (r->subject_type),
            r->subject,
            cf_acl_perm_to_level (r->permission),
            r->inherit));
    }

    return rules;
}

static void
run_case (const Case *tcase)
{
    GHashTable *subjects = build_subjects (tcase);
    GList *rules = build_rules (tcase);
    int i;

    for (i = 0; i < tcase->n_checks; i++) {
        const CaseCheck *check = &tcase->checks[i];
        char *got = cf_acl_resolve (rules, subjects, check->path,
                                    check->native);
        checks++;

        if (g_strcmp0 (got, check->expect) != 0) {
            failures++;
            fprintf (stderr,
                     "FAIL %s\n  path=%s native=%s\n  expected=%s got=%s\n",
                     tcase->name,
                     check->path ? check->path : "(empty)",
                     check->native ? check->native : "(null)",
                     check->expect ? check->expect : "(null)",
                     got ? got : "(null)");
        }

        g_free (got);
    }

    g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);
    g_hash_table_destroy (subjects);
}

/*
 * The eligibility invariant (v3, Pro compatible), checked exhaustively over
 * the lattice: a native of none stays none whatever the rules say -- a
 * directory rule never manufactures access. Inside a grant the rule defines
 * the value (r -> rw promotion included), so the old "never wider than
 * native" bound no longer holds by design.
 */
static void
test_never_widens (void)
{
    const char *decisions[] = { "invisible", "none", "r", "rw" };
    int d;

    for (d = 0; d < 4; d++) {
        GHashTable *subjects = g_hash_table_new_full (
            g_str_hash, g_str_equal, g_free, NULL);
        g_hash_table_add (subjects,
                          cf_acl_subject_key (CF_SUBJ_USER, "u@e.com"));

        GList *rules = g_list_append (NULL, cf_acl_rule_new (
            "/x", CF_SUBJ_USER, "u@e.com",
            cf_acl_perm_to_level (decisions[d]), 1));

        char *got = cf_acl_resolve (rules, subjects, "/x", NULL);
        checks++;

        if (got != NULL) {
            failures++;
            fprintf (stderr,
                     "FAIL invariant: native=none decision=%s got=%s\n",
                     decisions[d], got);
        }

        g_free (got);
        g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);
        g_hash_table_destroy (subjects);
    }
}

static void
expect_restricted (const char *label, GList *rules, GHashTable *subjects,
                   const char *root, const char *native, const char *expect)
{
    char *got = cf_acl_find_restricted (rules, subjects, root, native);
    checks++;

    if (g_strcmp0 (got, expect) != 0) {
        failures++;
        fprintf (stderr, "FAIL %s\n  root=%s\n  expected=%s got=%s\n",
                 label, root,
                 expect ? expect : "(null)", got ? got : "(null)");
    }

    g_free (got);
}

/*
 * A subtree handed to the client in one go -- a sync, a zip download -- is
 * blocked by a single unreadable descendant.
 */
static void
test_find_restricted (void)
{
    GHashTable *subjects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);
    g_hash_table_add (subjects, cf_acl_subject_key (CF_SUBJ_USER, "b@e.com"));

    GList *rules = NULL;
    rules = g_list_append (rules, cf_acl_rule_new (
        "/open", CF_SUBJ_USER, "b@e.com", CF_PERM_RW, 1));
    rules = g_list_append (rules, cf_acl_rule_new (
        "/secret/deep", CF_SUBJ_USER, "b@e.com", CF_PERM_INVISIBLE, 1));

    expect_restricted ("whole repo is blocked by a hidden descendant",
                       rules, subjects, "/", "rw", "/secret/deep");
    expect_restricted ("the branch containing the rule is blocked",
                       rules, subjects, "/secret", "rw", "/secret/deep");
    expect_restricted ("an unaffected branch is fine",
                       rules, subjects, "/open", "rw", NULL);
    expect_restricted ("prefix collision does not block",
                       rules, subjects, "/secretive", "rw", NULL);
    expect_restricted ("the restricted path itself reports itself",
                       rules, subjects, "/secret/deep", "rw", "/secret/deep");

    g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);
    g_hash_table_destroy (subjects);
}

int
main (void)
{
    int i;

    for (i = 0; i < CF_ACL_N_CASES; i++)
        run_case (&cf_acl_cases[i]);

    test_never_widens ();
    test_find_restricted ();

    printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
