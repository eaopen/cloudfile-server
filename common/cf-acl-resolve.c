/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <string.h>

#include "cf-acl-resolve.h"
#include "cf-path.h"

CfAclRule *
cf_acl_rule_new (const char *path,
                 int subject_type,
                 const char *subject,
                 int permission,
                 int inherit)
{
    CfAclRule *rule = g_new0 (CfAclRule, 1);
    rule->path = cf_acl_normalize_path (path);
    rule->subject_type = subject_type;
    rule->subject = g_strdup (subject);
    rule->permission = permission;
    rule->inherit = inherit;
    return rule;
}

void
cf_acl_rule_free (CfAclRule *rule)
{
    if (!rule)
        return;
    g_free (rule->path);
    g_free (rule->subject);
    g_free (rule);
}

int
cf_acl_perm_to_level (const char *perm)
{
    if (!perm)
        return CF_PERM_UNKNOWN;
    if (strcmp (perm, "invisible") == 0)
        return CF_PERM_INVISIBLE;
    if (strcmp (perm, "none") == 0)
        return CF_PERM_NONE;
    if (strcmp (perm, "r") == 0)
        return CF_PERM_R;
    if (strcmp (perm, "rw") == 0)
        return CF_PERM_RW;
    return CF_PERM_UNKNOWN;
}

static const char *
level_to_perm (int level)
{
    switch (level) {
    case CF_PERM_R:  return "r";
    case CF_PERM_RW: return "rw";
    default:         return NULL;
    }
}

int
cf_acl_subject_type_to_level (const char *subject_type)
{
    if (!subject_type)
        return CF_SUBJ_UNKNOWN;
    if (strcmp (subject_type, "user") == 0)
        return CF_SUBJ_USER;
    if (strcmp (subject_type, "dept") == 0)
        return CF_SUBJ_DEPT;
    if (strcmp (subject_type, "group") == 0)
        return CF_SUBJ_GROUP;
    return CF_SUBJ_UNKNOWN;
}

char *
cf_acl_subject_key (int subject_type, const char *subject)
{
    return g_strdup_printf ("%d:%s", subject_type, subject);
}

/*
 * Moved to common/cf-path.c when the write lifecycle seam needed the same
 * rules: the ACL keys rules by path and the lock keys leases by path, and if
 * the two normalized differently a rule on /a/b and a lock on /a/b/ would be
 * about different objects. One implementation, so there is nothing to drift.
 *
 * Kept as a forwarder rather than renaming the call sites so the ACL's own
 * tests and case set stayed untouched by a baseline refactor.
 */
char *
cf_acl_normalize_path (const char *path)
{
    return cf_path_normalize (path);
}

GList *
cf_acl_ancestors (const char *path)
{
    char *norm = cf_acl_normalize_path (path);
    GList *levels = g_list_append (NULL, g_strdup ("/"));

    if (strcmp (norm, "/") == 0) {
        g_free (norm);
        return levels;
    }

    GString *current = g_string_new ("");
    const char *p = norm + 1;   /* skip the leading '/' */

    while (*p) {
        const char *start = p;
        while (*p && *p != '/')
            p++;
        g_string_append_c (current, '/');
        g_string_append_len (current, start, p - start);
        levels = g_list_append (levels, g_strdup (current->str));
        if (*p == '/')
            p++;
    }

    g_string_free (current, TRUE);
    g_free (norm);
    return levels;
}

static gboolean
subject_matches (GHashTable *subjects, const CfAclRule *rule)
{
    char *key = cf_acl_subject_key (rule->subject_type, rule->subject);
    gboolean found = g_hash_table_contains (subjects, key);
    g_free (key);
    return found;
}

/*
 * Pick the winning permission among the rules that matched at one level:
 * most specific subject type first, then -- within that type -- a deny
 * vetoes outright and otherwise the highest grant wins.
 *
 * v3 note: resolve() feeds this function one track at a time (personal, or
 * dept/group), so the user level here only settles dept-vs-group conflicts
 * and the deny-wins rule inside a single type. Splitting by subject type is
 * what keeps an explicit user grant meaningful; within the same type a deny
 * ('invisible'/'none') vetoes any grant (CE 'none' semantics), while
 * competing grants merge to the highest ('rw' over 'r'), matching CE's group
 * permission merge in repo-perm.c.
 */
static int
pick_level_decision (GList *applicable)
{
    int best_type = CF_SUBJ_UNKNOWN;
    GList *ptr;

    for (ptr = applicable; ptr; ptr = ptr->next) {
        CfAclRule *rule = ptr->data;
        if (rule->subject_type > best_type)
            best_type = rule->subject_type;
    }

    int deny = CF_PERM_UNKNOWN;    /* most restrictive deny seen */
    int grant = CF_PERM_UNKNOWN;   /* highest grant seen */
    gboolean found = FALSE;

    for (ptr = applicable; ptr; ptr = ptr->next) {
        CfAclRule *rule = ptr->data;
        if (rule->subject_type != best_type)
            continue;
        found = TRUE;

        if (rule->permission == CF_PERM_INVISIBLE)
            return CF_PERM_INVISIBLE;   /* strictest value; nothing out-ranks it */
        if (rule->permission == CF_PERM_NONE) {
            deny = CF_PERM_NONE;
        } else if (rule->permission > grant) {
            grant = rule->permission;
        }
    }

    if (!found)
        return CF_PERM_UNKNOWN;
    if (deny == CF_PERM_NONE)
        return CF_PERM_NONE;
    return grant;
}

/*
 * Combine the resolved rule with the native share permission (v3, Pro
 * compatible).
 *
 * `native` still gates eligibility: NULL means no share reaches this user
 * and a rule never manufactures access. Within an existing grant the rule
 * *defines* the permission -- a library-level r may be promoted to rw on a
 * subtree (Seafile Pro sub-folder permissions) and rw demoted to r.
 * Denies still veto outright.
 */
static char *
tighten (const char *native_perm, int decision)
{
    if (!native_perm)
        return NULL;

    if (decision == CF_PERM_INVISIBLE || decision == CF_PERM_NONE)
        return NULL;

    int native_level = cf_acl_perm_to_level (native_perm);

    if (native_level == CF_PERM_R || native_level == CF_PERM_RW) {
        return g_strdup (level_to_perm (decision));
    }

    if (g_strcmp0 (native_perm, "admin") == 0) {
        /* admin outranks everything on the chain, so the rule always wins. */
        return g_strdup (level_to_perm (decision));
    }

    /* 'preview', 'cloud-edit' and custom permissions are not comparable with
     * r and rw -- one allows viewing without download, the other editing
     * without download. Ordering them would widen permission in some
     * direction, so they are only ever vetoed (handled above). */
    return g_strdup (native_perm);
}

/*
 * Deepest-level-with-matching-rules decision along the ancestor chain,
 * restricted to one subject-type track. NULL when the track has no rules.
 */
static int
resolve_track (GList *rules,
               GHashTable *subjects,
               const char *norm_path,
               GList *levels,
               gboolean personal)
{
    int decision = CF_PERM_UNKNOWN;
    GList *lp, *rp;

    for (lp = levels; lp; lp = lp->next) {
        const char *level = lp->data;
        GList *applicable = NULL;

        for (rp = rules; rp; rp = rp->next) {
            CfAclRule *rule = rp->data;
            gboolean is_personal = (rule->subject_type == CF_SUBJ_USER);
            if (is_personal != personal)
                continue;
            if (strcmp (rule->path, level) != 0)
                continue;
            if (!rule->inherit && strcmp (level, norm_path) != 0)
                continue;
            if (!subject_matches (subjects, rule))
                continue;
            applicable = g_list_prepend (applicable, rule);
        }

        if (applicable) {
            /* A level with matching rules replaces the inherited decision;
             * a level without any leaves it alone. */
            decision = pick_level_decision (applicable);
            g_list_free (applicable);
        }
    }

    return decision;
}

char *
cf_acl_resolve (GList *rules,
                GHashTable *subjects,
                const char *path,
                const char *native_perm)
{
    if (!native_perm)
        return NULL;

    char *norm_path = cf_acl_normalize_path (path);
    GList *levels = cf_acl_ancestors (norm_path);

    /*
     * v3 (Pro compatible): two tracks. The personal track carries CF_SUBJ_USER
     * rules and wins outright over the dept/group track whenever it produces a
     * decision -- across levels, so a personal rule inherited from a parent
     * directory beats a group rule on a deeper one. Within a track the deepest
     * matching level still wins.
     */
    int personal = resolve_track (rules, subjects, norm_path, levels, TRUE);
    int group = resolve_track (rules, subjects, norm_path, levels, FALSE);

    int decision = (personal != CF_PERM_UNKNOWN) ? personal : group;

    char *result;
    if (decision == CF_PERM_UNKNOWN)
        result = g_strdup (native_perm);
    else
        result = tighten (native_perm, decision);

    g_list_free_full (levels, g_free);
    g_free (norm_path);

    return result;
}

/* Is @path the same as @root, or below it? */
static gboolean
is_at_or_below (const char *path, const char *root)
{
    if (strcmp (root, "/") == 0)
        return TRUE;
    if (strcmp (path, root) == 0)
        return TRUE;

    size_t root_len = strlen (root);
    return strncmp (path, root, root_len) == 0 && path[root_len] == '/';
}

static gint
compare_paths (gconstpointer a, gconstpointer b)
{
    return strcmp ((const char *)a, (const char *)b);
}

char *
cf_acl_find_restricted (GList *rules,
                        GHashTable *subjects,
                        const char *root,
                        const char *native_perm)
{
    char *norm_root = cf_acl_normalize_path (root);
    GList *candidates = g_list_append (NULL, g_strdup (norm_root));
    GList *ptr;

    for (ptr = rules; ptr; ptr = ptr->next) {
        CfAclRule *rule = ptr->data;
        if (!is_at_or_below (rule->path, norm_root))
            continue;
        if (g_list_find_custom (candidates, rule->path, compare_paths))
            continue;
        candidates = g_list_append (candidates, g_strdup (rule->path));
    }

    /* Sorted so the reported path is stable across runs and DB orderings --
     * an error message that changes between identical requests is a support
     * problem. */
    candidates = g_list_sort (candidates, compare_paths);

    char *restricted = NULL;
    for (ptr = candidates; ptr; ptr = ptr->next) {
        const char *candidate = ptr->data;
        char *perm = cf_acl_resolve (rules, subjects, candidate, native_perm);
        if (!perm) {
            restricted = g_strdup (candidate);
            break;
        }
        g_free (perm);
    }

    g_list_free_full (candidates, g_free);
    g_free (norm_root);

    return restricted;
}
