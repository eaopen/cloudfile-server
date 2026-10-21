/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "common.h"
#include "cloudfile-policy.h"
#include "cloudfile-acl.h"
#include "cf-lock.h"
#include "seafile-session.h"
#include "seafile-error.h"
#include "repo-mgr.h"
#include "fs-mgr.h"
#include "utils.h"
#include <sys/stat.h>
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

gboolean
cf_policy_legacy_guard_enabled (GKeyFile *config)
{
    if (!config || !g_key_file_has_key (config, "cloudfile", "managed_library_guard", NULL))
        return FALSE;
    GError *error = NULL;
    gboolean enabled = g_key_file_get_boolean (config, "cloudfile", "managed_library_guard", &error);
    /* An invalid configured value must never silently open the old paths. */
    if (error) { g_error_free (error); return TRUE; }
    return enabled;
}

gboolean
cf_policy_allow_legacy_managed_reads (GKeyFile *config)
{
    if (!config || !g_key_file_has_key (config, "cloudfile", "allow_legacy_managed_reads", NULL))
        return FALSE;
    GError *error = NULL;
    gboolean enabled = g_key_file_get_boolean (config, "cloudfile", "allow_legacy_managed_reads", &error);
    if (error) { g_error_free (error); return FALSE; }
    return enabled;
}

/* Current V2 directory ACL is the only native whole-library boundary.
 * Never infer policy from retired managed-library tables or a migration ledger. */
gboolean
cf_policy_managed_guard_required (SeafDB *db, GKeyFile *config)
{
    (void)db;
    (void)config;
    return TRUE;
}

static gboolean
managed_origin (SeafDBRow *row, void *data)
{
    char **origin = data;
    *origin = g_strdup (seaf_db_row_get_column_text (row, 0));
    return TRUE;
}

/* One decision, two outputs: refuse/enroll and, when refusing, whether the
 * runtime could actually read its authoritative state. A deliberate
 * managed-library boundary and an unreadable schema/SQL failure both refuse,
 * but only the latter is an availability fault -- reporting it as "no
 * permission" hides an outage behind an authorization answer. */
static int
cf_policy_legacy_library_decision (SeafDBTrans *trans, const char *repo,
                                   gboolean *unavailable)
{
    if (unavailable) *unavailable = FALSE;
    if (!trans || !repo || strlen (repo) != 36) {
        if (unavailable) *unavailable = TRUE;
        return -1;
    }
    char *origin = NULL;
    int count = seaf_db_trans_foreach_selected_row (trans,
        "SELECT origin_repo FROM VirtualRepo WHERE repo_id=? FOR UPDATE",
        managed_origin, &origin, 1, "string", repo);
    if (count < 0 || count > 1 || (count == 1 && (!origin || strlen (origin) != 36))) {
        g_free (origin);
        if (unavailable) *unavailable = TRUE;
        return -1;
    }
    const char *first = repo, *second = origin;
    if (second && strcmp (first, second) > 0) { first = origin; second = repo; }
    gboolean error = FALSE;
    gboolean blocked = seaf_db_trans_check_for_existence (trans,
        "SELECT repo_id FROM cf_dir_acl WHERE repo_id=? LIMIT 1 FOR UPDATE", &error,
        1, "string", first);
    if (!blocked && !error && second)
        blocked = seaf_db_trans_check_for_existence (trans,
            "SELECT repo_id FROM cf_dir_acl WHERE repo_id=? LIMIT 1 FOR UPDATE", &error,
            1, "string", second);
    g_free (origin);
    if (error) {
        if (unavailable) *unavailable = TRUE;
        return -1;
    }
    /* Reached only with a readable schema: this is the boundary itself. */
    return blocked ? -1 : 0;
}

int
cf_policy_check_legacy_library (SeafDBTrans *trans, const char *repo)
{
    return cf_policy_legacy_library_decision (trans, repo, NULL);
}

int
cf_policy_check_legacy_access_reason (SeafDB *db, GKeyFile *config,
                                      const char *repo, gboolean *unavailable)
{
    if (unavailable) *unavailable = FALSE;
    if (!cf_policy_managed_guard_required (db, config)) return 0;
    SeafDBTrans *trans = seaf_db_begin_transaction (db);
    if (!trans) {
        /* The guard is required but no transaction can be opened. */
        if (unavailable) *unavailable = TRUE;
        return -1;
    }
    int result = cf_policy_legacy_library_decision (trans, repo, unavailable);
    seaf_db_rollback (trans);
    seaf_db_trans_close (trans);
    return result;
}

int
cf_policy_check_legacy_access (SeafDB *db, GKeyFile *config, const char *repo)
{
    return cf_policy_check_legacy_access_reason (db, config, repo, NULL);
}

/* Retired control plane. V2 ACL is managed directly by Hub; no enrollment. */
int
cf_policy_enroll_managed_library (SeafDBTrans *trans, const char *repo)
{
    (void)trans;
    (void)repo;
    return -1;
}

int
cf_policy_check_unleased_write (SeafDBTrans *trans, const char *repo,
                                const char *path, const char *user)
{
    if (!cf_lock_enabled ()) return 0;
    (void)user;
    if (!repo || !path || path[0] != '/' || strlen (path) > 4096 ||
        !g_utf8_validate (path, -1, NULL)) return -1;
    char *digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    if (!digest) return -1;
    /* V2 publishes against one path-keyed Guard, in the Branch transaction.
     * The retired Resource/Intent ledger must not be a file-write dependency.
     * Unscoped owner writes also refuse an active exclusive reservation. */
    gboolean error = FALSE;
    gboolean guarded = seaf_db_trans_check_for_existence (trans,
        "SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) "
        "WHERE repo_id=? AND path_hash=UNHEX(?) AND path=? "
        "AND guard_id IS NOT NULL AND lease_expires_at>UTC_TIMESTAMP(6) FOR UPDATE",
        &error, 3, "string", repo, "string", digest, "string", path);
    g_free (digest);
    return guarded || error ? -1 : 0;
}

int
cf_policy_check_lease_write (SeafDBTrans *trans, const char *repo, const char *path,
                             const char *user, json_t *proof)
{
    /* Retired lease proofs cannot authorize a controlled editing publication. */
    if (proof) return -1;
    return cf_policy_check_unleased_write (trans, repo, path, user);
}

/* Historical Resource/CommitIntent publisher retired. V2 Go owns CAS. */
int
cf_policy_edit_publish (SeafDBTrans *trans, const char *repo, const char *path,
                        const char *user, json_t *conditions, const char *commit_id)
{
    (void)trans; (void)repo; (void)path; (void)user;
    (void)conditions; (void)commit_id;
    return -1;
}

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

typedef struct {
    const char *repo, *path, *object_id;
    gboolean valid;
    json_t *snapshot;
} LocalSessionTarget;

/* Retired Session/Device lookup. V2 Agent2 persists no server-side session. */
int
cf_policy_check_local_session (SeafDBTrans *trans, const char *repo,
                               const char *path, const char *object_id,
                               int kind, json_t *root,
                               json_t *current_subject, int ce_permission)
{
    (void)trans; (void)repo; (void)path; (void)object_id;
    (void)kind; (void)root; (void)current_subject; (void)ce_permission;
    return -1;
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

typedef struct {
    GArray *rules;
    GPtrArray *owned;
    GHashTable *paths;
    gboolean valid;
} RuleRows;

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
    const char *share_mode = seaf_db_row_get_column_text (row, 10);
    const char *birth = seaf_db_row_get_column_text (row, 11);
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
    if (g_strcmp0 (share_mode, "0") && g_strcmp0 (share_mode, "1")) goto invalid;
    if (!strcmp (share_mode, "1")) {
        if (rule.kind != CF_DIRECTORY || !strcmp (path, "/") || !valid_id (birth, 512)) goto invalid;
    } else if (!birth || *birth) goto invalid;
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

/* Verify the same bounded path-birth evidence used by Hub NativeResourceReader.
 * Every commit is immutable; Branch is pinned for the entire caller transaction.
 * A missing historical path ends this incarnation, never authorizes a new one. */
static gboolean directory_head_row (SeafDBRow *row, void *data)
{
    char **head = data;
    g_free (*head);
    *head = g_strdup (seaf_db_row_get_column_text (row, 0));
    return TRUE;
}

static int directory_birth_matches (SeafDBTrans *trans, const char *repo_id,
                                    const char *path, const char *expected)
{
    const char *prefix = "ce14:path-birth:";
    char *head = NULL, *birth = NULL;
    SeafRepo *repo = NULL;
    int result = -1;
    gint64 deadline = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;
    if (!expected || !g_str_has_prefix (expected, prefix) ||
        !is_object_id_valid (expected + strlen (prefix)) || !seaf) return -1;
    if (seaf_db_trans_foreach_selected_row (trans,
        "SELECT commit_id FROM Branch WHERE repo_id=? AND name='master' FOR UPDATE",
        directory_head_row, &head, 1, "string", repo_id) != 1 ||
        !head || !is_object_id_valid (head)) goto out;
    repo = seaf_repo_manager_get_repo (seaf->repo_mgr, repo_id);
    if (!repo || repo->virtual_info || repo->encrypted) goto out;
    for (int depth = 0; depth < 256; ++depth) {
        if (g_get_monotonic_time () >= deadline) goto out;
        SeafCommit *commit = seaf_commit_manager_get_commit (seaf->commit_mgr,
            repo_id, repo->version, head);
        if (!commit) goto out;
        if (commit->second_parent_id) { seaf_commit_unref (commit); goto out; }
        GError *error = NULL;
        guint32 mode = 0;
        char *object = seaf_fs_manager_path_to_obj_id (seaf->fs_mgr, repo->store_id,
            repo->version, commit->root_id, path, &mode, &error);
        gboolean failed = error && !g_error_matches (error, SEAFILE_DOMAIN, SEAF_ERR_PATH_NO_EXIST);
        if (error) g_error_free (error);
        gboolean exists = object != NULL && S_ISDIR (mode);
        g_free (object);
        if (failed) { seaf_commit_unref (commit); goto out; }
        if (!exists) {
            result = birth && !strcmp (birth, expected + strlen (prefix)) ? 1 : 0;
            seaf_commit_unref (commit);
            goto out;
        }
        g_free (birth); birth = g_strdup (head);
        g_free (head); head = g_strdup (commit->parent_id);
        seaf_commit_unref (commit);
        if (!head || !*head) {
            result = !strcmp (birth, expected + strlen (prefix)) ? 1 : 0;
            goto out;
        }
        if (!is_object_id_valid (head)) goto out;
    }
out:
    if (repo) seaf_repo_unref (repo);
    g_free (head); g_free (birth);
    return result;
}

typedef struct {
    GArray *shares;
    GPtrArray *owned, *lifecycles;
    GHashTable *paths, *subjects, *births;
    gboolean valid;
    SeafDBTrans *trans;
    const char *repo;
} DirectoryShareRows;

static gboolean directory_share_row (SeafDBRow *row, void *data)
{
    DirectoryShareRows *rows = data;
    const char *path = seaf_db_row_get_column_text (row, 0);
    const char *path_hash = seaf_db_row_get_column_text (row, 1);
    const char *birth = seaf_db_row_get_column_text (row, 2);
    const char *type = seaf_db_row_get_column_text (row, 3);
    const char *provider = seaf_db_row_get_column_text (row, 4);
    const char *namespace = seaf_db_row_get_column_text (row, 5);
    const char *external = seaf_db_row_get_column_text (row, 6);
    const char *subject_hash = seaf_db_row_get_column_text (row, 7);
    char *id = NULL, *hash = NULL;
    struct cf_directory_share share = {0};
    if (!path || !strcmp (path, "/") || !g_hash_table_contains (rows->paths, path) ||
        !valid_id (provider, 32) || !valid_id (namespace, 255) ||
        !valid_id (external, 255) || !valid_id (birth, 512)) goto invalid;
    hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    if (g_strcmp0 (hash, path_hash)) goto invalid;
    g_free (hash); hash = NULL;
    share.subject_type = !g_strcmp0 (type, "user") ? CF_USER :
        !g_strcmp0 (type, "dept") ? CF_DEPT : !g_strcmp0 (type, "group") ? CF_GROUP : -1;
    if (share.subject_type < 0 || (share.subject_type == CF_USER &&
        (strcmp (namespace, "user") || !valid_id (external, 225)))) goto invalid;
    id = subject_id (type, provider, namespace, external);
    if (!id) goto invalid;
    hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, id, -1);
    if (g_strcmp0 (hash, subject_hash)) goto invalid;
    if (g_hash_table_contains (rows->subjects, id)) {
        share.path = g_strdup (path); share.subject_id = id;
        g_ptr_array_add (rows->owned, (gpointer)share.path);
        g_ptr_array_add (rows->owned, id); id = NULL;
        g_ptr_array_add (rows->lifecycles, g_strdup (birth));
        g_array_append_val (rows->shares, share);
    }
    g_free (hash); free (id);
    return TRUE;
invalid:
    g_free (hash); free (id); rows->valid = FALSE; return FALSE;
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
        !valid_id (user, 225) || (ce_permission < 0 || ce_permission > 2)) return -1;
    RuleRows rows = {g_array_new (FALSE, FALSE, sizeof (struct cf_acl_rule)),
                    g_ptr_array_new_with_free_func (g_free),
                    g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL), TRUE};
    DirectoryShareRows shared = {g_array_new (FALSE, FALSE, sizeof (struct cf_directory_share)),
        rows.owned, g_ptr_array_new_with_free_func (g_free), rows.paths, g_hash_table_new (g_str_hash, g_str_equal),
        g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL), TRUE, trans, repo};
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
        "SELECT path,path_hash,kind,subject_type,provider,namespace,external_id,subject_hash,permission,inherit,share_mode,lifecycle_ref "
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
    /* Do not convert a directory qualification into library permission. */
    g_hash_table_add (shared.subjects, identity);
    for (guint i = 0; i < departments->len; ++i) g_hash_table_add (shared.subjects, departments->pdata[i]);
    for (guint i = 0; i < groups->len; ++i) g_hash_table_add (shared.subjects, groups->pdata[i]);
    /* Sharing is a projection of the same ACL row, never an independent grant.
     * The first query already pinned and verified the ACL index contract. */
    query = g_string_new ("SELECT path,path_hash,lifecycle_ref,subject_type,provider,namespace,external_id,subject_hash "
        "FROM cf_dir_acl FORCE INDEX (acl_ancestors) WHERE share_mode=1 AND kind='dir' AND repo_id=? AND path_hash IN (");
    first = TRUE;
    g_hash_table_iter_init (&iter, rows.paths);
    while (g_hash_table_iter_next (&iter, &candidate, NULL)) {
        char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, candidate, -1);
        g_string_append_printf (query, "%s'%s'", first ? "" : ",", hash);
        g_free (hash); first = FALSE;
    }
    g_string_append (query, ") AND (inherit=1 OR path=?) ORDER BY path_hash,id LIMIT 4097 FOR UPDATE");
    g_free (sql); sql = g_string_free (query, FALSE);
    count = seaf_db_trans_foreach_selected_row (trans, sql, directory_share_row, &shared, 2, "string", repo, "string", path);
    if (count < 0 || count > 4096 || !shared.valid) goto out;
    /* Finish the SQL result before issuing lifecycle queries on this connection. */
    guint kept = 0;
    for (guint i = 0; i < shared.shares->len; ++i) {
        struct cf_directory_share item = g_array_index (shared.shares, struct cf_directory_share, i);
        const char *birth = shared.lifecycles->pdata[i];
        char *key = g_strdup_printf ("%s\n%s", item.path, birth);
        gpointer stored = g_hash_table_lookup (shared.births, key);
        int matches = stored ? GPOINTER_TO_INT (stored) - 2 :
            directory_birth_matches (trans, repo, item.path, birth);
        if (!stored) g_hash_table_insert (shared.births, key, GINT_TO_POINTER (matches + 2));
        else g_free (key);
        if (matches < 0) goto out;
        if (matches) g_array_index (shared.shares, struct cf_directory_share, kept++) = item;
    }
    g_array_set_size (shared.shares, kept);
    int repo_status = -1;
    if (seaf_db_trans_foreach_selected_row (trans,
        "SELECT status FROM RepoInfo WHERE repo_id=? FOR UPDATE", schema_count, &repo_status,
        1, "string", repo) != 1 || (repo_status != 0 && repo_status != 1)) goto out;
    struct cf_acl_context context = {identity, (const char *const *)departments->pdata, departments->len,
        (const char *const *)groups->pdata, groups->len,
        ce_permission == 2 ? CF_WRITE : ce_permission == 1 ? CF_READ : CF_NONE, 1, 1, repo_status == 1, 0};
    struct cf_acl_result evaluated;
    if (cf_acl_evaluate_scoped (&context, path, kind, (struct cf_acl_rule *)rows.rules->data,
                         rows.rules->len, (struct cf_directory_share *)shared.shares->data, shared.shares->len, &evaluated) == 0 &&
        (write ? evaluated.write : evaluated.read)) result = 0;
out:
    g_hash_table_destroy (shared.subjects); g_hash_table_destroy (shared.births); g_array_free (shared.shares, TRUE); g_ptr_array_free (shared.lifecycles, TRUE);
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

int cf_policy_check_create (SeafDBTrans *trans, const char *repo, const char *path,
                           const char *provider, const char *user, json_t *snapshot,
                           int ce_permission)
{
    if (!path || path[0] != '/' || !strcmp (path, "/")) return -1;
    char *parent = g_path_get_dirname (path);
    int allowed = policy_check (trans, repo, parent, provider, user, snapshot,
                                ce_permission, CF_DIRECTORY, TRUE);
    g_free (parent);
    return allowed == 0 ? cf_policy_check_write (trans, repo, path, provider,
                                                user, snapshot, ce_permission) : -1;
}

/* Legacy Hub optimistic Intent/Session receipts are not part of V2. */
int
cf_policy_optimistic_receipt (SeafDBTrans *trans, const char *repo,
                              json_t *conditions, const char *commit_id)
{
    (void)trans; (void)repo; (void)conditions; (void)commit_id;
    return -1;
}

/* Legacy device-commit proof path is not registered in V2. */
int
cf_policy_check_device_commit (SeafDBTrans *trans, const char *repo,
                               const char *path, json_t *conditions,
                               json_t *subject, int qualification)
{
    (void)trans; (void)repo; (void)path;
    (void)conditions; (void)subject; (void)qualification;
    return -1;
}
