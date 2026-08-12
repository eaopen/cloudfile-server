/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <string.h>

#include "log.h"
#include "seafile-session.h"
#include "seaf-db.h"
#include "group-mgr.h"
#include "cf-ext.h"
#include "cf-acl.h"

static gboolean cf_acl_on = FALSE;

void
cf_acl_init (void)
{
    if (!seaf || !seaf->cfg_mgr) {
        cf_acl_on = FALSE;
        return;
    }

    cf_acl_on = cf_ext_config_bool ("dir_acl_enabled");
    if (!cf_acl_on)
        return;

    /*
     * Register with the extension table rather than having rpc-service.c call
     * cf_acl_* directly.
     *
     * The three upstream files that dispatch permission decisions
     * (rpc-service.c, seaf-server.c, seafile-session.c) are patched once, by
     * the baseline, to call into cf-ext.c. If this capability reached into
     * them itself, every other capability would have to as well, and they
     * would all collide on the same lines -- first between branches, then
     * permanently on dev.
     *
     * Registering only when the switch is on is what keeps a disabled ACL
     * indistinguishable from stock CE: with an empty provider table every
     * cf_ext_* dispatch is a pass-through, so there is no ACL code on the hot
     * path at all rather than a function that returns early.
     */
    cf_ext_register ("dir-acl",
                     cf_acl_apply,
                     cf_acl_filter_dirents,
                     cf_acl_find_restricted_path);
}

gboolean
cf_acl_enabled (void)
{
    return cf_acl_on;
}

/* -- subjects ------------------------------------------------------------ */

/*
 * The subject set for a user: their address, their groups, their departments.
 *
 * get_groups_by_user with return_ancestors=TRUE already walks the department
 * tree upwards, which is what makes a rule on a parent department apply to
 * members of its sub-departments (acl-semantics.md section 3).
 */
static GHashTable *
build_subject_set (const char *user)
{
    GHashTable *subjects = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                  g_free, NULL);

    g_hash_table_add (subjects, cf_acl_subject_key (CF_SUBJ_USER, user));

    GList *groups = ccnet_group_manager_get_groups_by_user (seaf->group_mgr,
                                                            user, TRUE, NULL);
    GList *ptr;
    for (ptr = groups; ptr; ptr = ptr->next) {
        CcnetGroup *group = ptr->data;
        int group_id = 0, parent_group_id = 0;
        g_object_get (group, "id", &group_id,
                      "parent_group_id", &parent_group_id, NULL);

        /* parent_group_id: 0 = ordinary group, anything else = department
         * (-1 marks a top-level one, >0 points at the parent). */
        int type = (parent_group_id == 0) ? CF_SUBJ_GROUP : CF_SUBJ_DEPT;
        char *id_str = g_strdup_printf ("%d", group_id);
        g_hash_table_add (subjects, cf_acl_subject_key (type, id_str));
        g_free (id_str);
    }
    g_list_free_full (groups, g_object_unref);

    return subjects;
}

/* -- rule loading -------------------------------------------------------- */

static gboolean
load_rule_cb (SeafDBRow *row, void *data)
{
    GList **rules = data;

    const char *path = seaf_db_row_get_column_text (row, 0);
    const char *subject_type = seaf_db_row_get_column_text (row, 1);
    const char *subject = seaf_db_row_get_column_text (row, 2);
    const char *permission = seaf_db_row_get_column_text (row, 3);
    int inherit = seaf_db_row_get_column_int (row, 4);

    int type_level = cf_acl_subject_type_to_level (subject_type);
    int perm_level = cf_acl_perm_to_level (permission);
    if (type_level == CF_SUBJ_UNKNOWN || perm_level == CF_PERM_UNKNOWN) {
        /* Skip rather than guess: a row we cannot interpret must not be
         * silently treated as "allow". */
        seaf_warning ("CloudFile: ignoring ACL row with unknown "
                      "subject_type '%s' or permission '%s'.\n",
                      subject_type ? subject_type : "(null)",
                      permission ? permission : "(null)");
        return TRUE;
    }

    *rules = g_list_prepend (*rules,
                             cf_acl_rule_new (path, type_level, subject,
                                              perm_level, inherit));
    return TRUE;
}

/*
 * Every rule in a repo, in one query.
 *
 * A permission check walks each ancestor of the path, so fetching the whole
 * repo once beats one query per level. Results are deliberately not cached:
 * this is the authoritative check, and a cache would leave a window in which
 * a revoked permission still worked. Seahub does cache, which at worst means
 * it shows an entry the server then refuses.
 *
 * Sets @db_error when the query itself failed, so the caller can tell "no
 * rules" apart from "could not read the rules".
 */
static GList *
load_repo_rules (const char *repo_id, gboolean *db_error)
{
    GList *rules = NULL;
    const char *sql =
        "SELECT path, subject_type, subject, permission, inherit "
        "FROM cf_dir_acl WHERE repo_id = ?";

    *db_error = FALSE;

    if (seaf_db_statement_foreach_row (seaf->db, sql, load_rule_cb, &rules,
                                       1, "string", repo_id) < 0) {
        seaf_warning ("CloudFile: failed to load directory ACL for repo %s.\n",
                      repo_id);
        g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);
        *db_error = TRUE;
        return NULL;
    }

    return rules;
}

char *
cf_acl_apply (const char *repo_id,
              const char *path,
              const char *user,
              const char *native_perm)
{
    if (!cf_acl_on || !native_perm)
        return g_strdup (native_perm);

    if (!repo_id || !user || *user == '\0')
        return g_strdup (native_perm);

    gboolean db_error = FALSE;
    GList *rules = load_repo_rules (repo_id, &db_error);
    if (db_error) {
        /* Fail closed. Falling open here would hand out access precisely when
         * the rules that restrict it cannot be read. */
        return NULL;
    }
    if (!rules)
        return g_strdup (native_perm);

    GHashTable *subjects = build_subject_set (user);

    char *result = cf_acl_resolve (rules, subjects, path, native_perm);

    g_hash_table_destroy (subjects);
    g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);

    return result;
}

GList *
cf_acl_filter_dirents (const char *repo_id,
                       const char *dir_path,
                       const char *user,
                       GList *dirents)
{
    if (!cf_acl_on || !dirents)
        return dirents;

    if (!repo_id || !user || *user == '\0')
        return dirents;

    gboolean db_error = FALSE;
    GList *rules = load_repo_rules (repo_id, &db_error);
    if (db_error) {
        /* Fail closed: show nothing rather than risk listing what an
         * unreadable rule set was meant to hide. */
        g_list_free_full (dirents, g_object_unref);
        return NULL;
    }
    if (!rules)
        return dirents;

    GHashTable *subjects = build_subject_set (user);
    char *norm_dir = cf_acl_normalize_path (dir_path);

    GList *kept = NULL, *ptr;
    for (ptr = dirents; ptr; ptr = ptr->next) {
        GObject *dirent = ptr->data;
        char *name = NULL, *native_perm = NULL;
        g_object_get (dirent, "obj_name", &name,
                      "permission", &native_perm, NULL);

        char *child_path;
        if (strcmp (norm_dir, "/") == 0)
            child_path = g_strdup_printf ("/%s", name ? name : "");
        else
            child_path = g_strdup_printf ("%s/%s", norm_dir, name ? name : "");

        char *perm = cf_acl_resolve (rules, subjects, child_path,
                                     native_perm);
        if (perm) {
            g_object_set (dirent, "permission", perm, NULL);
            kept = g_list_prepend (kept, dirent);
            g_free (perm);
        } else {
            g_object_unref (dirent);
        }

        g_free (child_path);
        g_free (name);
        g_free (native_perm);
    }

    g_list_free (dirents);
    g_free (norm_dir);
    g_hash_table_destroy (subjects);
    g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);

    return g_list_reverse (kept);
}

char *
cf_acl_find_restricted_path (const char *repo_id,
                             const char *path,
                             const char *user,
                             const char *native_perm)
{
    if (!cf_acl_on || !native_perm)
        return NULL;

    if (!repo_id || !user || *user == '\0')
        return NULL;

    gboolean db_error = FALSE;
    GList *rules = load_repo_rules (repo_id, &db_error);
    if (db_error) {
        /* Fail closed: report the root as restricted so the sync or download
         * is refused rather than allowed while the rules are unreadable. */
        return cf_acl_normalize_path (path);
    }
    if (!rules)
        return NULL;

    GHashTable *subjects = build_subject_set (user);

    char *restricted = cf_acl_find_restricted (rules, subjects, path,
                                               native_perm);

    g_hash_table_destroy (subjects);
    g_list_free_full (rules, (GDestroyNotify)cf_acl_rule_free);

    return restricted;
}
