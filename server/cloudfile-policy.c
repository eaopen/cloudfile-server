/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "common.h"
#include "cloudfile-policy.h"
#include "cloudfile-acl.h"
#include "cf-lock.h"
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

gboolean
cf_policy_managed_guard_required (SeafDB *db, GKeyFile *config)
{
    if (cf_policy_legacy_guard_enabled (config)) return TRUE;
    if (!db) return TRUE;
    if (seaf_db_type (db) != SEAF_DB_TYPE_MYSQL) return FALSE;
    gboolean error = FALSE;
    gboolean installed = seaf_db_statement_exists (db,
        "SELECT table_name FROM information_schema.tables WHERE table_schema=DATABASE() "
        "AND table_name IN ('cf_schema_migration','cf_managed_library')", &error, 0);
    return installed || error;
}

static gboolean
managed_isolation (SeafDBRow *row, void *data)
{
    gboolean *valid = data;
    const char *isolation = seaf_db_row_get_column_text (row, 0);
    *valid = g_strcmp0 (isolation, "REPEATABLE-READ") == 0 ||
        g_strcmp0 (isolation, "SERIALIZABLE") == 0;
    return TRUE;
}

static int
managed_schema (SeafDBTrans *trans)
{
    gboolean error = FALSE;
    /* Pin the table before metadata inspection and use current locking reads. */
    if (seaf_db_trans_foreach_selected_row (trans,
            "SELECT repo_id FROM cf_managed_library LIMIT 0 FOR UPDATE", NULL, NULL, 0) < 0)
        return -1;
    const char *checks[] = {
        "SELECT version FROM cf_schema_migration WHERE version='032_managed_libraries' AND state='applied'",
        "SELECT table_name FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name='cf_managed_library' AND ENGINE='InnoDB'",
        "SELECT table_name FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='cf_managed_library' GROUP BY table_name HAVING COUNT(*)=2 AND SUM(column_name='repo_id' AND data_type='char' AND character_maximum_length=36 AND collation_name='ascii_bin' AND is_nullable='NO')=1 AND SUM(column_name='created_at' AND data_type='datetime' AND datetime_precision=6 AND is_nullable='NO')=1",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_managed_library' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='repo_id' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1"
    };
    for (size_t i = 0; i < G_N_ELEMENTS (checks); ++i)
        if (!seaf_db_trans_check_for_existence (trans, checks[i], &error, 0) || error) return -1;
    /* Reuse qualification's MySQL/MariaDB variable compatibility. */
    gboolean isolation = FALSE;
    int rows = seaf_db_trans_foreach_selected_row (trans,
        "SELECT @@transaction_isolation", managed_isolation, &isolation, 0);
    if (rows < 0)
        rows = seaf_db_trans_foreach_selected_row (trans,
            "SELECT @@tx_isolation", managed_isolation, &isolation, 0);
    return rows == 1 && isolation ? 0 : -1;
}

static gboolean
managed_origin (SeafDBRow *row, void *data)
{
    char **origin = data;
    *origin = g_strdup (seaf_db_row_get_column_text (row, 0));
    return TRUE;
}

int
cf_policy_check_legacy_library (SeafDBTrans *trans, const char *repo)
{
    if (!repo || strlen (repo) != 36 || managed_schema (trans) < 0) return -1;
    char *origin = NULL;
    int count = seaf_db_trans_foreach_selected_row (trans,
        "SELECT origin_repo FROM VirtualRepo WHERE repo_id=? FOR UPDATE",
        managed_origin, &origin, 1, "string", repo);
    if (count < 0 || count > 1 || (count == 1 && (!origin || strlen (origin) != 36))) {
        g_free (origin); return -1;
    }
    const char *first = repo, *second = origin;
    if (second && strcmp (first, second) > 0) { first = origin; second = repo; }
    gboolean error = FALSE;
    gboolean blocked = seaf_db_trans_check_for_existence (trans,
        "SELECT repo_id FROM cf_managed_library WHERE repo_id=? FOR UPDATE", &error,
        1, "string", first);
    if (!blocked && !error && second)
        blocked = seaf_db_trans_check_for_existence (trans,
            "SELECT repo_id FROM cf_managed_library WHERE repo_id=? FOR UPDATE", &error,
            1, "string", second);
    g_free (origin);
    return !blocked && !error ? 0 : -1;
}

int
cf_policy_check_legacy_access (SeafDB *db, GKeyFile *config, const char *repo)
{
    if (!cf_policy_managed_guard_required (db, config)) return 0;
    SeafDBTrans *trans = seaf_db_begin_transaction (db);
    if (!trans) return -1;
    int result = cf_policy_check_legacy_library (trans, repo);
    seaf_db_rollback (trans);
    seaf_db_trans_close (trans);
    return result;
}

int
cf_policy_enroll_managed_library (SeafDBTrans *trans, const char *repo)
{
    if (!repo || strlen (repo) != 36 || managed_schema (trans) < 0) return -1;
    return seaf_db_trans_query (trans,
        "INSERT INTO cf_managed_library(repo_id,created_at) VALUES(?,UTC_TIMESTAMP(6)) "
        "ON DUPLICATE KEY UPDATE repo_id=VALUES(repo_id)", 1, "string", repo);
}

static int editing_schema (SeafDBTrans *trans)
{
    gboolean error = FALSE;
    return seaf_db_trans_check_for_existence (trans,
        "SELECT version FROM cf_schema_migration WHERE version='029_editing_core' "
        "AND state='applied' AND step=2 FOR UPDATE", &error, 0) && !error ? 0 : -1;
}

typedef struct {
    const char *path;
    const char *expected_uid;
    gboolean valid;
} LeaseResourceLocation;

static gboolean
lease_resource_row (SeafDBRow *row, void *data)
{
    LeaseResourceLocation *location = data;
    const char *uid = seaf_db_row_get_column_text (row, 0);
    const char *path = seaf_db_row_get_column_text (row, 1);
    if (!uid || strlen (uid) != 36 || g_strcmp0 (path, location->path) ||
        (location->expected_uid && g_strcmp0 (uid, location->expected_uid)))
        location->valid = FALSE;
    return TRUE;
}

static int
lease_resource_location (SeafDBTrans *trans, const char *repo,
                         const char *path, const char *hash, const char *uid)
{
    LeaseResourceLocation location = {path, uid, TRUE};
    /* Hash is an indexed locator, never identity. Read at most two CURRENT
     * rows before joining a lease: a collision or duplicate active lifecycle
     * must not be hidden by the UID-specific join or EXISTS short circuit.
     * The repository authority scope serializes supported resource mutations.
     * This does not replace native lifecycle reconciliation. */
    int count = seaf_db_trans_foreach_selected_row (trans,
        "SELECT uid,path FROM cf_resource WHERE repo_id=? AND path_hash=? "
        "AND kind='file' AND state='active' LIMIT 2 FOR UPDATE",
        lease_resource_row, &location, 2, "string", repo, "string", hash);
    return count >= 0 && count <= 1 && location.valid && (!uid || count == 1) ? 0 : -1;
}

/* Only the opt-in lock barrier requires the new checkout schema. The final
 * Branch transaction must re-read it after PREPARE, closing that race for
 * scoped file publication. A missing or malformed table fails closed. */
static int
editing_conflict (SeafDBTrans *trans, const char *repo, const char *path,
                  const char *hash, const char *user)
{
    gboolean error = FALSE;
    gboolean guarded = seaf_db_trans_check_for_existence (trans,
        "SELECT r.uid FROM cf_resource r JOIN cf_edit_guard g ON g.resource_uid=r.uid "
        "WHERE r.repo_id=? AND r.path_hash=? AND r.path=? AND r.state='active' "
        "AND g.repo_id=r.repo_id AND g.guard_id IS NOT NULL "
        "AND (g.lifecycle_ref<>r.lifecycle_ref OR g.mode IS NULL OR g.mode<>'file-lock' OR g.owner_native_user IS NULL OR g.owner_native_user<>?) FOR UPDATE",
        &error, 4, "string", repo, "string", hash, "string", path,
        "string", user ? user : "");
    return guarded || error ? -1 : 0;
}

int
cf_policy_check_unleased_write (SeafDBTrans *trans, const char *repo,
                                const char *path, const char *user)
{
    if (!cf_lock_enabled ()) return 0;
    if (!repo || !path || path[0] != '/' || strlen (path) > 4096 ||
        !g_utf8_validate (path, -1, NULL) || editing_schema (trans) < 0) return -1;
    char *digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    if (!digest) return -1;
    if (lease_resource_location (trans, repo, path, digest, NULL) < 0) {
        g_free (digest);
        return -1;
    }
    /* Current locking read, not a preflight/RR snapshot. The actual sparse UID
     * and lease rows stay locked until the caller's Branch transaction ends.
     * SQL errors fail closed; orphan/lifecycle reconciliation remains a
     * separate native integration prerequisite, not proven by this join.
     * Exact stored path is compared after hashing to avoid digest-only identity.
     */
    int result = editing_conflict (trans, repo, path, digest, user);
    g_free (digest);
    return result;
}

int
cf_policy_check_lease_write (SeafDBTrans *trans, const char *repo, const char *path,
                             const char *user, json_t *proof)
{
    /* Retired lease proofs cannot authorize a controlled editing publication. */
    if (proof) return -1;
    return cf_policy_check_unleased_write (trans, repo, path, user);
}

typedef struct {
    char action[24];
} EditPublication;

static gboolean
edit_publication_row (SeafDBRow *row, void *data)
{
    EditPublication *publication = data;
    const char *action = seaf_db_row_get_column_text (row, 0);
    if (g_strcmp0 (action, "commit") && g_strcmp0 (action, "checkin") &&
        g_strcmp0 (action, "checkin-unchanged"))
        return FALSE;
    g_strlcpy (publication->action, action, sizeof(publication->action));
    return TRUE;
}

static const char *
edit_text (json_t *object, const char *name, size_t maximum)
{
    json_t *value = json_object_get (object, name);
    const char *text = json_string_value (value);
    if (!json_is_string (value) || !text || !*text ||
        json_string_length (value) != strlen (text) || strlen (text) > maximum)
        return NULL;
    return text;
}

static gboolean
edit_decimal (const char *value)
{
    if (!value || !*value || *value == '0' || strlen (value) > 20)
        return FALSE;
    for (const char *p = value; *p; ++p)
        if (!g_ascii_isdigit (*p)) return FALSE;
    errno = 0;
    guint64 number = g_ascii_strtoull (value, NULL, 10);
    char canonical[32];
    g_snprintf (canonical, sizeof(canonical), "%" G_GUINT64_FORMAT, number);
    return !errno && strcmp (canonical, value) == 0;
}

int
cf_policy_edit_publish (SeafDBTrans *trans, const char *repo, const char *path,
                        const char *user, json_t *conditions,
                        const char *commit_id)
{
    if (!cf_lock_enabled () || !trans || !repo || !path || !user ||
        !json_is_object (conditions) || editing_schema (trans) < 0)
        return -1;
    json_t *edit = json_object_get (conditions, "editing");
    if (!json_is_object (edit) || json_object_size (edit) != 9 ||
        g_strcmp0 (edit_text (edit, "repo_id", 36), repo))
        return -1;
    const char *resource_uid = edit_text (edit, "resource_uid", 36);
    const char *guard = edit_text (edit, "guard_id", 36);
    const char *generation = edit_text (edit, "generation", 20);
    const char *epoch = edit_text (edit, "credential_epoch", 20);
    const char *holder = edit_text (edit, "holder", 128);
    const char *token = edit_text (edit, "token", 64);
    const char *intent = edit_text (edit, "intent_id", 36);
    const char *base_id = edit_text (edit, "base_file_id", 40);
    const char *old_id = edit_text (conditions, "native_old_file_id", 40);
    const char *new_id = edit_text (conditions, "native_file_id", 40);
    const char *content = edit_text (conditions, "native_digest", 64);
    json_t *size = json_object_get (conditions, "native_size");
    if (!json_is_integer (size) || json_integer_value (size) < 0) return -1;
    json_t *context = json_object_get (conditions, "context");
    const char *owner = edit_text (context, "userId", 225);
    if (!resource_uid || strlen (resource_uid) != 36 ||
        !guard || !edit_decimal (generation) || !edit_decimal (epoch) ||
        !holder || !token || strlen (token) != 64 || !intent || !owner ||
        !base_id || strlen (base_id) != 40 ||
        !old_id || strlen (old_id) != 40 || strcmp (old_id, base_id) ||
        !new_id || strlen (new_id) != 40 ||
        !content || strlen (content) != 64 ||
        !g_utf8_validate (path, -1, NULL) || path[0] != '/' || strlen (path) > 4096 ||
        (commit_id && strlen (commit_id) != 40)) return -1;
    for (const char *p = token; *p; ++p)
        if (!g_ascii_isxdigit (*p)) return -1;
    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    char *token_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, token, -1);
    EditPublication publication = {{0}};
    int count = seaf_db_trans_foreach_selected_row (trans,
        "SELECT i.action FROM cf_resource r JOIN cf_edit_guard g ON g.resource_uid=r.uid "
        "JOIN cf_commit_intent i ON i.intent_id=g.pending_intent AND i.resource_uid=r.uid "
        "WHERE r.repo_id=? AND r.uid=? AND r.path_hash=? AND r.path=? AND r.kind='file' AND r.state='active' "
        "AND g.repo_id=r.repo_id AND g.lifecycle_ref=r.lifecycle_ref AND g.mode='checkout' "
        "AND g.guard_id=? AND g.generation=CAST(? AS UNSIGNED) "
        "AND g.credential_epoch=CAST(? AS UNSIGNED) "
        "AND g.owner=? "
        "AND g.owner_native_user=? AND g.holder=? AND g.proof_digest=? "
        "AND g.lease_until>UTC_TIMESTAMP(6) AND g.hard_expire_at>UTC_TIMESTAMP(6) "
        "AND i.intent_id=? AND i.guard_id=g.guard_id AND i.generation=g.generation "
        "AND i.credential_epoch=g.credential_epoch AND i.state='prepared' "
        "AND i.expected_file_id=? AND i.expected_file_id=g.base_file_id "
        "AND (i.staged_file_id IS NULL OR i.staged_file_id=?) "
        "AND i.content_digest=? AND (i.action='checkin-unchanged' OR (i.snapshot IS NOT NULL AND JSON_EXTRACT(i.snapshot,'$.size')=CAST(? AS UNSIGNED))) LIMIT 2 FOR UPDATE",
        edit_publication_row, &publication, 16,
        "string", repo, "string", resource_uid, "string", path_hash, "string", path,
        "string", guard, "string", generation, "string", epoch,
        "string", owner,
        "string", user, "string", holder, "string", token_hash,
        "string", intent, "string", old_id, "string", new_id,
        "string", content, "int64", (gint64)json_integer_value (size));
    g_free (path_hash);
    g_free (token_hash);
    if (count != 1 || !publication.action[0]) return -1;
    gboolean unchanged = json_is_true (json_object_get (conditions, "editing_unchanged"));
    if (unchanged != (strcmp (publication.action, "checkin-unchanged") == 0) ||
        (unchanged && (strcmp (old_id, new_id) ||
                       strcmp (content, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"))))
        return -1;
    if (!commit_id) return 0;
    if (seaf_db_trans_query (trans,
            "UPDATE cf_commit_intent SET state='published',staged_file_id=?,"
            "result_file_id=?,result_commit_id=? WHERE intent_id=? AND state='prepared'",
            4, "string", new_id, "string", new_id, "string", commit_id,
            "string", intent) < 0) return -1;
    if (!strcmp (publication.action, "checkin") || unchanged)
        return seaf_db_trans_query (trans,
            "UPDATE cf_edit_guard SET guard_id=NULL,"
            "mode=NULL,owner=NULL,owner_native_user=NULL,source=NULL,holder=NULL,credential_epoch=0,"
            "proof_digest=NULL,base_file_id=NULL,pending_intent=NULL,lease_until=NULL,"
            "hard_expire_at=NULL WHERE guard_id=? AND pending_intent=?",
            2, "string", guard, "string", intent);
    return seaf_db_trans_query (trans,
        "UPDATE cf_edit_guard SET base_file_id=?,pending_intent=NULL "
        "WHERE guard_id=? AND pending_intent=?",
        3, "string", new_id, "string", guard, "string", intent);
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

typedef struct {
    const char *repo, *path, *object_id;
    gboolean valid;
    json_t *snapshot;
} LocalSessionTarget;

static int local_session_schema (SeafDBTrans *trans)
{
    gboolean error = FALSE;
    const char *pins[] = {
        "SELECT session_id FROM cf_edit_session LIMIT 0 FOR UPDATE",
        "SELECT device_id FROM cf_local_device LIMIT 0 FOR UPDATE",
        "SELECT uid FROM cf_resource LIMIT 0 FOR UPDATE"
    };
    for (size_t i = 0; i < G_N_ELEMENTS (pins); ++i) {
        seaf_db_trans_check_for_existence (trans, pins[i], &error, 0);
        if (error) return -1;
    }
    const char *checks[] = {
        "SELECT table_name FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='cf_edit_session' GROUP BY table_name HAVING COUNT(*)=13 AND SUM("
        "(column_name IN ('session_id','device_id') AND data_type='char' AND character_maximum_length=36 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='provider' AND data_type='varchar' AND character_maximum_length=32 AND collation_name='utf8mb4_bin' AND is_nullable='NO') OR "
        "(column_name='owner_user_id' AND data_type='varchar' AND character_maximum_length=225 AND collation_name='utf8mb4_bin' AND is_nullable='NO') OR "
        "(column_name IN ('device_revision','revision','ticket_expires_at','expires_at') AND data_type='bigint' AND column_type LIKE '%unsigned' AND is_nullable='NO') OR "
        "(column_name='snapshot' AND data_type='longtext' AND collation_name='utf8mb4_bin' AND is_nullable='NO') OR "
        "(column_name='state' AND data_type='varchar' AND character_maximum_length=12 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='ticket_digest' AND data_type='char' AND character_maximum_length=64 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name IN ('created_at','updated_at') AND data_type='datetime' AND datetime_precision=6 AND is_nullable='NO'))=13",
        "SELECT table_name FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='cf_local_device' GROUP BY table_name HAVING COUNT(*)=9 AND SUM("
        "(column_name='device_id' AND data_type='char' AND character_maximum_length=36 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='provider' AND data_type='varchar' AND character_maximum_length=32 AND collation_name='utf8mb4_bin' AND is_nullable='NO') OR "
        "(column_name='owner_user_id' AND data_type='varchar' AND character_maximum_length=225 AND collation_name='utf8mb4_bin' AND is_nullable='NO') OR "
        "(column_name IN ('key_x','key_y','key_thumbprint') AND data_type='char' AND character_maximum_length=43 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='state' AND data_type='varchar' AND character_maximum_length=8 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='revision' AND data_type='bigint' AND column_type LIKE '%unsigned' AND is_nullable='NO') OR "
        "(column_name='updated_at' AND data_type='datetime' AND datetime_precision=6 AND is_nullable='NO'))=9",
        "SELECT table_name FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='cf_resource' GROUP BY table_name HAVING SUM("
        "(column_name IN ('uid','repo_id') AND data_type='char' AND character_maximum_length=36 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='path' AND data_type='longtext' AND collation_name='utf8mb4_bin' AND is_nullable='NO') OR "
        "(column_name='path_hash' AND data_type='char' AND character_maximum_length=64 AND character_set_name='ascii' AND is_nullable='NO') OR "
        "(column_name='kind' AND data_type='varchar' AND character_maximum_length=8 AND character_set_name='ascii' AND is_nullable='NO') OR "
        "(column_name='state' AND data_type='varchar' AND character_maximum_length=16 AND character_set_name='ascii' AND is_nullable='NO') OR "
        "(column_name='lifecycle_ref' AND data_type='varchar' AND character_maximum_length=512 AND collation_name='utf8mb4_bin' AND is_nullable='NO') OR "
        "(column_name='local_open_type' AND data_type='varchar' AND character_maximum_length=64 AND character_set_name='ascii' AND is_nullable='YES'))=8",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_edit_session' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='session_id' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_local_device' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='device_id' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_local_device' AND index_name='owner_key' GROUP BY index_name HAVING COUNT(*)=3 AND SUM(sub_part IS NULL AND non_unique=0 AND ((column_name='provider' AND seq_in_index=1) OR (column_name='owner_user_id' AND seq_in_index=2) OR (column_name='key_thumbprint' AND seq_in_index=3)))=3",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_resource' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='uid' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_resource' AND index_name='resource_location' GROUP BY index_name HAVING COUNT(*)=3 AND SUM(sub_part IS NULL AND non_unique=1 AND ((column_name='repo_id' AND seq_in_index=1) OR (column_name='path_hash' AND seq_in_index=2) OR (column_name='kind' AND seq_in_index=3)))=3"
    };
    for (size_t i = 0; i < G_N_ELEMENTS (checks); ++i)
        if (!seaf_db_trans_check_for_existence (trans, checks[i], &error, 0) || error) return -1;
    return 0;
}

static gboolean local_session_target (SeafDBRow *row, void *data)
{
    LocalSessionTarget *target = data;
    const char *encoded = seaf_db_row_get_column_text (row, 0);
    json_t *snapshot = encoded && strlen (encoded) <= 16384 ?
        json_loads (encoded, JSON_REJECT_DUPLICATES, NULL) : NULL;
    json_t *resource = json_object_get (snapshot, "resource");
    const char *mode = json_identifier (snapshot, "mode", 32);
    target->valid = json_is_object (snapshot) && json_object_size (snapshot) == 8 &&
        json_is_object (resource) && json_object_size (resource) == 3 &&
        !g_strcmp0 (json_identifier (resource, "repo_id", 36), target->repo) &&
        !g_strcmp0 (json_identifier (resource, "path", 4096), target->path) &&
        !g_strcmp0 (json_identifier (resource, "kind", 8), "file") &&
        !g_strcmp0 (json_identifier (snapshot, "base_version", 40), target->object_id) &&
        !g_strcmp0 (json_identifier (snapshot, "resource_uid", 36),
                    seaf_db_row_get_column_text (row, 1)) &&
        !g_strcmp0 (json_identifier (snapshot, "lifecycle_ref", 512),
                    seaf_db_row_get_column_text (row, 2)) &&
        (json_is_string (json_object_get (snapshot, "local_open_type"))) &&
        json_string_length (json_object_get (snapshot, "local_open_type")) ==
            strlen (json_string_value (json_object_get (snapshot, "local_open_type"))) &&
        !g_strcmp0 (json_string_value (json_object_get (snapshot, "local_open_type")),
                    seaf_db_row_get_column_text (row, 3)) &&
        (mode && (!strcmp (mode, "view") || !strcmp (mode, "optimistic-edit") ||
                  !strcmp (mode, "exclusive-edit")));
    if (target->snapshot) json_decref (target->snapshot);
    target->snapshot = snapshot;
    return TRUE;
}

static int local_session_lease (SeafDBTrans *trans, const char *repo,
                                const char *user, json_t *snapshot)
{
    (void)trans; (void)repo; (void)user;
    const char *mode = json_identifier (snapshot, "mode", 32);
    return g_strcmp0 (mode, "exclusive-edit") &&
           json_is_null (json_object_get (snapshot, "lease")) ? 0 : -1;
}

int cf_policy_check_local_session (SeafDBTrans *trans, const char *repo,
                                  const char *path, const char *object_id,
                                  int kind, json_t *root,
                                  json_t *current_subject, int ce_permission)
{
    json_t *proof = json_object_get (root, "local_session");
    if (!proof) return 0;
    json_t *context = json_object_get (root, "context");
    const char *session = json_identifier (proof, "session_id", 36);
    const char *device = json_identifier (proof, "device_id", 36);
    const char *device_revision = json_identifier (proof, "device_revision", 20);
    const char *session_revision = json_identifier (proof, "session_revision", 20);
    const char *provider = json_identifier (context, "provider", 32);
    const char *user = json_identifier (context, "userId", 225);
    if (!trans || kind != CF_FILE || !json_is_object (proof) || json_object_size (proof) != 4 ||
        !session || !device || !device_revision || !session_revision || !provider || !user ||
        strlen (session) != 36 || strlen (device) != 36 ||
        device_revision[0] == '0' || session_revision[0] == '0' ||
        strspn (device_revision, "0123456789") != strlen (device_revision) ||
        strspn (session_revision, "0123456789") != strlen (session_revision)) return -1;
    gboolean error = FALSE;
    if (!seaf_db_trans_check_for_existence (trans,
        "SELECT version FROM cf_schema_migration WHERE version='030_local_devices' "
        "AND state='applied' AND step=2 FOR UPDATE", &error, 0) || error) return -1;
    if (!seaf_db_trans_check_for_existence (trans,
        "SELECT version FROM cf_schema_migration WHERE version='031_edit_sessions' "
        "AND state='applied' AND step=2 FOR UPDATE", &error, 0) || error) return -1;
    if (!seaf_db_trans_check_for_existence (trans,
        "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME IN ('cf_edit_session','cf_local_device','cf_resource') "
        "AND ENGINE='InnoDB' HAVING COUNT(*)=3", &error, 0) || error) return -1;
    if (local_session_schema (trans) < 0) return -1;
    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    if (!path_hash || lease_resource_location (trans, repo, path, path_hash, NULL) < 0) {
        g_free (path_hash);
        return -1;
    }
    LocalSessionTarget target = { repo, path, object_id, FALSE, NULL };
    int count = seaf_db_trans_foreach_selected_row (trans,
        "SELECT s.snapshot,r.uid,r.lifecycle_ref,COALESCE(r.local_open_type,'') "
        "FROM cf_edit_session s JOIN cf_local_device d ON d.device_id=s.device_id "
        "JOIN cf_resource r ON r.repo_id=? AND r.path_hash=? AND r.path=? AND r.kind='file' AND r.state='active' "
        "WHERE s.session_id=? AND s.provider=? AND s.owner_user_id=? AND s.device_id=? "
        "AND d.provider=s.provider AND d.owner_user_id=s.owner_user_id AND d.state='active' "
        "AND d.revision=s.device_revision AND CAST(d.revision AS CHAR)=? "
        "AND CAST(s.revision AS CHAR)=? AND s.state IN ('claimed','active') "
        "AND s.expires_at>FLOOR(UNIX_TIMESTAMP()) LIMIT 2 FOR UPDATE",
        local_session_target, &target, 9, "string", repo, "string", path_hash, "string", path,
        "string", session, "string", provider, "string", user, "string", device,
        "string", device_revision, "string", session_revision);
    g_free (path_hash);
    int result = count == 1 && target.valid &&
        local_session_lease (trans, repo, user, target.snapshot) == 0 ? 0 : -1;
    /* An edit session cannot be used to start/continue an editable download
     * after its native library or directory write permission is withdrawn.
     * Use the caller's actual current subject, never the historical snapshot. */
    if (result == 0 && g_strcmp0 (json_identifier (target.snapshot, "mode", 32), "view") &&
        cf_policy_check_write (trans, repo, path, provider, user,
                               current_subject, ce_permission) < 0)
        result = -1;
    if (target.snapshot) json_decref (target.snapshot);
    return result;
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
