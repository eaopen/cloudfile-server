/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "common.h"
#include "cloudfile-policy.h"
#include "cloudfile-acl.h"
#include <string.h>
#include <stdlib.h>

int
cf_policy_check_unleased_write (SeafDBTrans *trans, const char *repo, const char *path)
{
    gboolean error = FALSE;
    if (!repo || !path || path[0] != '/' || strlen (path) > 4096 ||
        !g_utf8_validate (path, -1, NULL)) return -1;
    /* Missing/old lock schema is not an unlocked file. These metadata gates
     * supplement, not replace, Hub's exact constraint/checksum validation. */
    if (!seaf_db_trans_check_for_existence (trans,
        "SELECT version FROM cf_schema_migration WHERE version='029_lock_leases' AND state='applied' AND step=2 FOR UPDATE",
        &error, 0) || error) return -1;
    const char *tables[] = {"cf_resource", "cf_lock_lease", "cf_lock_repo_revision"};
    for (int i = 0; i < 3; ++i) {
        if (!seaf_db_trans_check_for_existence (trans,
            "SELECT table_name FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name=? AND engine='InnoDB'",
            &error, 1, "string", tables[i]) || error) return -1;
    }
    char *digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    if (!digest) return -1;
    /* Current locking read, not a preflight/RR snapshot. The actual sparse UID
     * and lease rows stay locked until the caller's Branch transaction ends.
     * Any unreadable/orphaned active lookup fails conservatively via SQL error.
     * Exact stored path is compared after hashing to avoid digest-only identity.
     */
    gboolean locked = seaf_db_trans_check_for_existence (trans,
        "SELECT r.uid FROM cf_resource r JOIN cf_lock_lease l ON l.resource_uid=r.uid "
        "WHERE r.repo_id=? AND r.path_hash=? AND r.path=? AND r.kind='file' AND r.state='active' "
        "AND l.repo_id=? AND l.expires_at>UTC_TIMESTAMP(6) FOR UPDATE",
        &error, 4, "string", repo, "string", digest, "string", path, "string", repo);
    g_free (digest);
    return locked || error ? -1 : 0;
}

typedef struct {
    GArray *rules;
    GPtrArray *owned;
    GHashTable *paths;
    gboolean valid;
} RuleRows;

static gboolean valid_id (const char *value, int maximum)
{
    return value && *value && g_utf8_validate (value, -1, NULL) &&
           g_utf8_strlen (value, -1) <= maximum;
}

static char *subject_id (const char *type, const char *provider,
                        const char *namespace, const char *external)
{
    json_t *value = json_pack ("{s:s,s:s,s:s,s:s}", "type", type, "provider", provider,
                              "namespace", namespace, "external_id", external);
    if (!value) return NULL;
    char *encoded = json_dumps (value, JSON_COMPACT | JSON_SORT_KEYS);
    json_decref (value);
    return encoded;
}

static const char *json_identifier (json_t *item, const char *key, int maximum)
{
    json_t *value = json_object_get (item, key);
    const char *text = json_string_value (value);
    return json_is_string (value) && json_string_length (value) == strlen (text) &&
           valid_id (text, maximum) ? text : NULL;
}

static int memberships (json_t *items, const char *type, const char *provider,
                        GPtrArray *values, GHashTable *seen)
{
    if (!json_is_array (items) || json_array_size (items) + values->len > 4096) return -1;
    for (size_t i = 0; i < json_array_size (items); ++i) {
        json_t *item = json_array_get (items, i);
        const char *namespace = json_identifier (item, "namespace", 255);
        const char *external = json_identifier (item, "external_id", 255);
        if (!namespace || !external) return -1;
        char *id = subject_id (type, provider, namespace, external);
        if (!id || g_hash_table_contains (seen, id)) { free (id); return -1; }
        g_ptr_array_add (values, id);
        g_hash_table_add (seen, id);
    }
    return 0;
}

static gboolean rule_row (SeafDBRow *row, void *data)
{
    RuleRows *rows = data;
    const char *path = seaf_db_row_get_column_text (row, 0);
    const char *path_hash = seaf_db_row_get_column_text (row, 1);
    const char *kind = seaf_db_row_get_column_text (row, 2);
    const char *type = seaf_db_row_get_column_text (row, 3);
    const char *provider = seaf_db_row_get_column_text (row, 4);
    const char *namespace = seaf_db_row_get_column_text (row, 5);
    const char *external = seaf_db_row_get_column_text (row, 6);
    const char *subject_hash = seaf_db_row_get_column_text (row, 7);
    const char *permission = seaf_db_row_get_column_text (row, 8);
    const char *inherit = seaf_db_row_get_column_text (row, 9);
    struct cf_acl_rule rule = {0};
    char *id = NULL, *hash = NULL;
    if (!path || !g_hash_table_contains (rows->paths, path) || strlen (path) > 4096 ||
        !valid_id (provider, 32) || !valid_id (namespace, 255) || !valid_id (external, 255) ||
        rows->rules->len >= 4096) goto invalid;
    hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    if (g_strcmp0 (hash, path_hash)) goto invalid;
    g_free (hash); hash = NULL;
    rule.kind = g_strcmp0 (kind, "dir") == 0 ? CF_DIRECTORY : g_strcmp0 (kind, "file") == 0 ? CF_FILE : -1;
    rule.subject_type = g_strcmp0 (type, "user") == 0 ? CF_USER : g_strcmp0 (type, "dept") == 0 ? CF_DEPT :
                        g_strcmp0 (type, "group") == 0 ? CF_GROUP : -1;
    rule.permission = g_strcmp0 (permission, "invisible") == 0 ? CF_INVISIBLE :
                      g_strcmp0 (permission, "none") == 0 ? CF_NONE :
                      g_strcmp0 (permission, "r") == 0 ? CF_READ : g_strcmp0 (permission, "rw") == 0 ? CF_WRITE : -1;
    rule.inherit = g_strcmp0 (inherit, "0") == 0 ? 0 : g_strcmp0 (inherit, "1") == 0 ? 1 : -1;
    if (rule.kind < 0 || rule.subject_type < 0 || rule.permission < 0 || rule.inherit < 0 ||
        (rule.kind == CF_FILE && (rule.inherit || rule.permission > CF_NONE)) ||
        (rule.subject_type == CF_USER && (strcmp (namespace, "user") || !valid_id (external, 225)))) goto invalid;
    id = subject_id (type, provider, namespace, external);
    if (!id) goto invalid;
    hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, id, -1);
    if (g_strcmp0 (hash, subject_hash)) goto invalid;
    /* Hash locates/verifies, complete JSON identity is compared by the ACL core. */
    rule.path = g_strdup (path); rule.subject_id = id;
    g_ptr_array_add (rows->owned, (gpointer)rule.path);
    g_ptr_array_add (rows->owned, id);
    g_array_append_val (rows->rules, rule);
    g_free (hash);
    return TRUE;
invalid:
    g_free (hash); free (id); rows->valid = FALSE; return FALSE;
}

static gboolean schema_count (SeafDBRow *row, void *data)
{
    int *value = data; *value = seaf_db_row_get_column_int (row, 0); return FALSE;
}

static int policy_check (SeafDBTrans *trans, const char *repo, const char *path,
                          const char *provider, const char *user, json_t *snapshot,
                          int ce_permission, int kind, gboolean write)
{
    int result = -1;
    if (!path || !g_utf8_validate (path, -1, NULL) || *path != '/' || strlen (path) > 4096 ||
        (kind != CF_FILE && kind != CF_DIRECTORY) ||
        (strlen (path) == 1 && kind != CF_DIRECTORY) ||
        (strlen (path) > 1 && path[strlen (path)-1] == '/') || !valid_id (provider, 32) ||
        !valid_id (user, 225) || (ce_permission != 1 && ce_permission != 2)) return -1;
    RuleRows rows = {g_array_new (FALSE, FALSE, sizeof (struct cf_acl_rule)),
                    g_ptr_array_new_with_free_func (g_free),
                    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL), TRUE};
    GPtrArray *departments = g_ptr_array_new_with_free_func (g_free), *groups = g_ptr_array_new_with_free_func (g_free);
    GHashTable *seen = g_hash_table_new (g_str_hash, g_str_equal);
    char *identity = subject_id ("user", provider, "user", user);
    char *current = g_strdup (path), *sql = NULL;
    json_t *subject = json_object_get (snapshot, "subject");
    if (!identity || !json_is_object (subject) || memberships (json_object_get (subject, "organizations"),
            "dept", provider, departments, seen) < 0) goto out;
    json_t *ancestors = json_object_get (subject, "organization_ancestors");
    if (ancestors && memberships (ancestors, "dept", provider, departments, seen) < 0) goto out;
    g_hash_table_remove_all (seen);
    if (memberships (json_object_get (subject, "roles"), "group", provider, groups, seen) < 0) goto out;
    int contract = 0;
    /* Pin metadata without scanning rows, before checking the index contract.
     * A prefix index must not turn an ostensibly bounded lookup into a scan. */
    if (seaf_db_trans_foreach_selected_row (trans,
        "SELECT id FROM cf_dir_acl LIMIT 0 FOR UPDATE", schema_count, &contract, 0) != 0) goto out;
    const char *verify = "SELECT IF("
        "(SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name='cf_dir_acl' AND engine='InnoDB')=1 AND "
        "(SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_dir_acl' AND index_name='acl_ancestors' AND sub_part IS NULL AND non_unique=1 AND ((seq_in_index=1 AND column_name='repo_id') OR (seq_in_index=2 AND column_name='path_hash') OR (seq_in_index=3 AND column_name='id')))=3 AND "
        "(SELECT COUNT(*) FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_dir_acl' AND index_name='acl_target_subject' AND sub_part IS NULL AND non_unique=0 AND ((seq_in_index=1 AND column_name='repo_id') OR (seq_in_index=2 AND column_name='path_hash') OR (seq_in_index=3 AND column_name='kind') OR (seq_in_index=4 AND column_name='subject_hash')))=4,1,0)";
    if (seaf_db_trans_foreach_selected_row (trans, verify, schema_count, &contract, 0) != 1 || contract != 1) goto out;
    int depth = 0;
    while (TRUE) {
        if (++depth > 129) goto out;
        g_hash_table_add (rows.paths, g_strdup (current));
        if (strcmp (current, "/") == 0) break;
        char *slash = strrchr (current, '/');
        if (slash == current) current[1] = '\0'; else *slash = '\0';
    }
    GString *query = g_string_new (
        "SELECT path,path_hash,kind,subject_type,provider,namespace,external_id,subject_hash,permission,inherit "
        "FROM cf_dir_acl FORCE INDEX (acl_ancestors) WHERE repo_id=? AND path_hash IN (");
    GHashTableIter iter; gpointer candidate; gboolean first = TRUE;
    g_hash_table_iter_init (&iter, rows.paths);
    while (g_hash_table_iter_next (&iter, &candidate, NULL)) {
        char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, candidate, -1);
        g_string_append_printf (query, "%s'%s'", first ? "" : ",", hash);
        g_free (hash); first = FALSE;
    }
    /* File deny applies only at the actual target, never an ancestor directory. */
    g_string_append (query, ") AND (kind='dir' OR (kind='file' AND path_hash=?)) ORDER BY path_hash,id LIMIT 4097 FOR UPDATE");
    sql = g_string_free (query, FALSE);
    char *target_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    int count = seaf_db_trans_foreach_selected_row (trans, sql, rule_row, &rows,
        2, "string", repo, "string", target_hash);
    g_free (target_hash);
    if (count < 0 || !rows.valid || count > 4096) goto out;
    struct cf_acl_context context = {identity, (const char *const *)departments->pdata, departments->len,
        (const char *const *)groups->pdata, groups->len,
        ce_permission == 2 ? CF_WRITE : CF_READ, 1, 1, 0, 0};
    struct cf_acl_result evaluated;
    if (cf_acl_evaluate (&context, path, kind, (struct cf_acl_rule *)rows.rules->data,
                         rows.rules->len, &evaluated) == 0 &&
        (write ? evaluated.write : evaluated.read)) result = 0;
out:
    g_hash_table_destroy (seen); g_ptr_array_free (departments, TRUE); g_ptr_array_free (groups, TRUE);
    g_hash_table_destroy (rows.paths); g_array_free (rows.rules, TRUE); g_ptr_array_free (rows.owned, TRUE);
    free (identity); g_free (current); g_free (sql);
    return result;
}

int cf_policy_check_write (SeafDBTrans *trans, const char *repo, const char *path,
                          const char *provider, const char *user, json_t *snapshot,
                          int ce_permission)
{
    return policy_check (trans, repo, path, provider, user, snapshot,
                         ce_permission, CF_FILE, TRUE);
}

int cf_policy_check_read (SeafDBTrans *trans, const char *repo, const char *path,
                         const char *provider, const char *user, json_t *snapshot,
                         int ce_permission, int kind)
{
    return policy_check (trans, repo, path, provider, user, snapshot,
                         ce_permission, kind, FALSE);
}
