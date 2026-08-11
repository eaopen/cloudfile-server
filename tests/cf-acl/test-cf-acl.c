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

/* The security invariant, checked exhaustively over the lattice. */
static void
test_never_widens (void)
{
    const char *natives[] = { "r", "rw" };
    const char *decisions[] = { "invisible", "none", "r", "rw" };
    int n, d;

    for (n = 0; n < 2; n++) {
        for (d = 0; d < 4; d++) {
            GHashTable *subjects = g_hash_table_new_full (
                g_str_hash, g_str_equal, g_free, NULL);
            g_hash_table_add (subjects,
                              cf_acl_subject_key (CF_SUBJ_USER, "u@e.com"));

            GList *rules = g_list_append (NULL, cf_acl_rule_new (
                "/x", CF_SUBJ_USER, "u@e.com",
                cf_acl_perm_to_level (decisions[d]), 1));

            char *got = cf_acl_resolve (rules, subjects, "/x", natives[n]);
            checks++;

            if (got && cf_acl_perm_to_level (got) >
                       cf_acl_perm_to_level (natives[n])) {
                failures++;
                fprintf (stderr,
                         "FAIL invariant: native=%s decision=%s got=%s\n",
                         natives[n], decisions[d], got);
            }

            g_free (got);
            g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);
            g_hash_table_destroy (subjects);
        }
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

/*
 * The three-state authority contract (SEC-02).
 *
 * cf-acl.c is what classifies authority states, and it needs the full seafile
 * build -- so the dependency-free harness cannot exercise the classifier
 * itself on macOS. What it CAN do is assert that the shared fixture is
 * self-consistent and that the canonical state names are present, because
 * the fixture is generated into this header by gen-cases.py. The behavioural
 * classification test runs under the Linux CI build (see plan 01-02).
 *
 * The single invariant that keeps an active authority outage from being
 * silently treated as "no restriction": no feature_enabled state may carry
 * verdict=passthrough. That is the elevation-of-privilege anti-pattern, and
 * this test fires if it ever creeps back into the fixture.
 */
static void
test_authority_states_contract (void)
{
    const char *const canonical[] = {
        "unsupported-stock", "inactive-disabled", "active-valid",
        "active-unavailable", "active-malformed", "active-stale",
    };
    gboolean seen[6] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE };
    int i;

    if (CF_ACL_N_AUTHORITY_STATES == 0) {
        failures++;
        fprintf (stderr,
                 "FAIL authority_states: fixture has no states; gen-cases.py "
                 "or acl-cases.json::authority_states is misconfigured.\n");
        return;
    }

    for (i = 0; i < CF_ACL_N_AUTHORITY_STATES; i++) {
        const AuthorityState *s = &cf_acl_authority_states[i];
        int canonical_idx = -1;
        int j;

        for (j = 0; j < 6; j++) {
            if (g_strcmp0 (s->name, canonical[j]) == 0) {
                canonical_idx = j;
                break;
            }
        }
        if (canonical_idx >= 0)
            seen[canonical_idx] = TRUE;
        checks++;

        /* The security invariant. This is the one test that fails if the
         * fixture ever lets an enabled authority pass through to native CE. */
        if (s->feature_enabled && g_strcmp0 (s->verdict, "passthrough") == 0) {
            failures++;
            fprintf (stderr,
                     "FAIL authority_states: state %s has feature_enabled=1 "
                     "but verdict=passthrough -- an enabled authority must "
                     "never silently delegate to native CE (SEC-02).\n",
                     s->name);
        }
        checks++;

        /* Only the two genuinely-off states may pass through. */
        if (!s->feature_enabled && g_strcmp0 (s->verdict, "passthrough") != 0) {
            failures++;
            fprintf (stderr,
                     "FAIL authority_states: state %s has feature_enabled=0 "
                     "but verdict=%s; a disabled authority MUST be "
                     "indistinguishable from stock CE.\n",
                     s->name, s->verdict ? s->verdict : "(null)");
        }
        checks++;
    }

    for (i = 0; i < 6; i++) {
        checks++;
        if (!seen[i]) {
            failures++;
            fprintf (stderr,
                     "FAIL authority_states: canonical state %s is missing "
                     "from the fixture; the spec and the consumers must move "
                     "together.\n",
                     canonical[i]);
        }
    }

    checks++;
    if (g_strcmp0 (CF_ACL_UNKNOWN_STATE_VERDICT, "deny") != 0) {
        failures++;
        fprintf (stderr,
                 "FAIL authority_states: an unknown state must deny, got=%s.\n",
                 CF_ACL_UNKNOWN_STATE_VERDICT);
    }
}

/*
 * Revision monotonicity. The persisted counter must never go backward; cache
 * invalidation keys off it, so a rollback would let a cached deny/allow
 * survive a rule change.
 */
static void
test_revision_monotonicity (void)
{
    int last = -1;
    int i;

    for (i = 0; i < CF_ACL_N_REVISION_STEPS; i++) {
        const RevisionStep *step = &cf_acl_revision_steps[i];
        checks++;

        if (step->after <= last) {
            failures++;
            fprintf (stderr,
                     "FAIL revision_monotonicity: step %d (%s) went %d -> %d; "
                     "revision must strictly increase.\n",
                     i, step->op ? step->op : "(null)", last, step->after);
        }
        last = step->after;
    }

    if (CF_ACL_N_REVISION_STEPS == 0) {
        failures++;
        fprintf (stderr,
                 "FAIL revision_monotonicity: fixture has no steps; "
                 "acl-cases.json::authority_states.revision_monotonicity is "
                 "missing.\n");
    }
}

int
main (void)
{
    int i;

    for (i = 0; i < CF_ACL_N_CASES; i++)
        run_case (&cf_acl_cases[i]);

    test_never_widens ();
    test_find_restricted ();
    test_authority_states_contract ();
    test_revision_monotonicity ();

    printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
