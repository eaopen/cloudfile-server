/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "common.h"
#include "cloudfile-policy.h"
#include "cloudfile-acl.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static int
lease_schema (SeafDBTrans *trans)
{
    gboolean error = FALSE;
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
    const char *pins[] = {
        "SELECT uid FROM cf_resource LIMIT 0 FOR UPDATE",
        "SELECT resource_uid FROM cf_lock_lease LIMIT 0 FOR UPDATE",
        "SELECT repo_id FROM cf_lock_repo_revision LIMIT 0 FOR UPDATE"
    };
    for (int i = 0; i < 3; ++i) {
        seaf_db_trans_check_for_existence (trans, pins[i], &error, 0);
        if (error) return -1;
    }
    const char *checks[] = {
        "SELECT table_name FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='cf_lock_lease' GROUP BY table_name HAVING COUNT(*)=8 AND SUM("
        "(column_name IN ('resource_uid','repo_id') AND data_type='char' AND character_maximum_length=36 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='fencing' AND data_type='bigint' AND column_type LIKE '%unsigned' AND is_nullable='NO') OR "
        "(column_name='owner_user_id' AND data_type='varchar' AND character_maximum_length=225 AND collation_name='utf8mb4_bin' AND is_nullable='YES') OR "
        "(column_name='holder_id' AND data_type='varchar' AND character_maximum_length=128 AND collation_name='utf8mb4_bin' AND is_nullable='YES') OR "
        "(column_name='token_digest' AND data_type='char' AND character_maximum_length=64 AND collation_name='ascii_bin' AND is_nullable='YES') OR "
        "(column_name='base_version' AND data_type='char' AND character_maximum_length=40 AND collation_name='ascii_bin' AND is_nullable='YES') OR "
        "(column_name='expires_at' AND data_type='datetime' AND datetime_precision=6 AND is_nullable='YES'))=8",
        "SELECT table_name FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='cf_lock_repo_revision' GROUP BY table_name HAVING COUNT(*)=2 AND SUM("
        "(column_name='repo_id' AND data_type='char' AND character_maximum_length=36 AND collation_name='ascii_bin' AND is_nullable='NO') OR "
        "(column_name='revision' AND data_type='bigint' AND column_type LIKE '%unsigned' AND is_nullable='NO'))=2",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_lock_lease' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='resource_uid' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_lock_lease' AND index_name='repo_leases' GROUP BY index_name HAVING COUNT(*)=2 AND SUM(sub_part IS NULL AND non_unique=1 AND ((column_name='repo_id' AND seq_in_index=1) OR (column_name='resource_uid' AND seq_in_index=2)))=2",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_lock_repo_revision' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='repo_id' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_resource' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='uid' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_resource' AND index_name='resource_location' GROUP BY index_name HAVING COUNT(*)=3 AND SUM(sub_part IS NULL AND non_unique=1 AND ((column_name='repo_id' AND seq_in_index=1) OR (column_name='path_hash' AND seq_in_index=2) OR (column_name='kind' AND seq_in_index=3)))=3"
    };
    for (size_t i = 0; i < G_N_ELEMENTS (checks); ++i)
        if (!seaf_db_trans_check_for_existence (trans, checks[i], &error, 0) || error) return -1;
    return 0;
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

int
cf_policy_check_unleased_write (SeafDBTrans *trans, const char *repo, const char *path)
{
    gboolean error = FALSE;
    if (!repo || !path || path[0] != '/' || strlen (path) > 4096 ||
        !g_utf8_validate (path, -1, NULL) || lease_schema (trans) < 0) return -1;
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
    gboolean locked = seaf_db_trans_check_for_existence (trans,
        "SELECT r.uid FROM cf_resource r JOIN cf_lock_lease l ON l.resource_uid=r.uid "
        "WHERE r.repo_id=? AND r.path_hash=? AND r.path=? AND r.kind='file' AND r.state='active' "
        "AND l.repo_id=? AND l.expires_at>UTC_TIMESTAMP(6) FOR UPDATE",
        &error, 4, "string", repo, "string", digest, "string", path, "string", repo);
    g_free (digest);
    return locked || error ? -1 : 0;
}

static const char *
lease_text (json_t *proof, const char *name, size_t length)
{
    json_t *value = json_object_get (proof, name);
    const char *text = json_string_value (value);
    return text && json_string_length (value) == length && strlen (text) == length ? text : NULL;
}

int
cf_policy_check_lease_write (SeafDBTrans *trans, const char *repo, const char *path,
                             const char *user, json_t *proof)
{
    if (!proof) return cf_policy_check_unleased_write (trans, repo, path);
    if (!json_is_object (proof) || json_object_size (proof) != 5 || !user || !*user ||
        !repo || !path || path[0] != '/' || strlen (path) > 4096 || !g_utf8_validate (path, -1, NULL)) return -1;
    const char *uid = lease_text (proof, "resource_uid", 36);
    const char *holder = lease_text (proof, "holder_id", 64);
    const char *token = lease_text (proof, "token", 64);
    const char *base = lease_text (proof, "base_version", 40);
    json_t *number = json_object_get (proof, "fencing");
    const char *fence = json_string_value (number);
    if (!uid || !holder || !token || !base || !fence || !*fence || fence[0] == '0' ||
        strlen (fence) != json_string_length (number) || strlen (fence) > 20 ||
        strspn (fence, "0123456789") != strlen (fence) ||
        strspn (holder, "0123456789abcdef") != 64 || strspn (token, "0123456789abcdef") != 64 ||
        strspn (base, "0123456789abcdef") != 40) return -1;
    for (int i = 0; i < 36; ++i) {
        gboolean dash = i == 8 || i == 13 || i == 18 || i == 23;
        if ((dash && uid[i] != '-') || (!dash && !strchr ("0123456789abcdef", uid[i]))) return -1;
    }
    errno = 0;
    char *end = NULL;
    guint64 value = g_ascii_strtoull (fence, &end, 10);
    if (errno || !end || *end || value == 0) return -1;
    /* Reuse the schema gate, but not its 'unleased' decision. A live matching
     * lease is expected here; SQL failures and missing schema still reject. */
    gboolean error = FALSE;
    if (lease_schema (trans) < 0) return -1;
    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    char *token_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, token, -1);
    if (!path_hash || !token_hash || lease_resource_location (trans, repo, path, path_hash, uid) < 0) {
        g_free (path_hash);
        g_free (token_hash);
        return -1;
    }
    gboolean matches = seaf_db_trans_check_for_existence (trans,
        "SELECT r.uid FROM cf_resource r JOIN cf_lock_lease l ON l.resource_uid=r.uid "
        "WHERE r.uid=? AND r.repo_id=? AND r.path_hash=? AND r.path=? AND r.kind='file' AND r.state='active' "
        "AND l.repo_id=? AND l.owner_user_id=? AND l.holder_id=? AND l.token_digest=? "
        "AND CAST(l.fencing AS CHAR)=? AND l.base_version=? AND l.expires_at>UTC_TIMESTAMP(6) FOR UPDATE",
        &error, 10, "string", uid, "string", repo, "string", path_hash, "string", path,
        "string", repo, "string", user, "string", holder, "string", token_hash, "string", fence, "string", base);
    g_free (path_hash);
    g_free (token_hash);
    return matches && !error ? 0 : -1;
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
    const char *mode = json_identifier (snapshot, "mode", 32);
    json_t *proof = json_object_get (snapshot, "lease");
    if (g_strcmp0 (mode, "exclusive-edit")) return json_is_null (proof) ? 0 : -1;
    const char *fencing = json_identifier (proof, "fencing", 20);
    const char *digest = json_identifier (proof, "token_digest", 64);
    const char *uid = json_identifier (snapshot, "resource_uid", 36);
    const char *base = json_identifier (snapshot, "base_version", 40);
    if (!json_is_object (proof) || json_object_size (proof) != 2 || !fencing ||
        fencing[0] == '0' || strspn (fencing, "0123456789") != strlen (fencing) ||
        !digest || strlen (digest) != 64 || strspn (digest, "0123456789abcdef") != 64 ||
        !uid || !base || lease_schema (trans) < 0) return -1;
    gboolean error = FALSE;
    /* The saved digest identifies the exact holder credential, not merely an
     * owner-wide lock. Release/reacquire, expiry or force-release invalidate an
     * already issued transfer, including when its file object is unchanged. */
    gboolean matches = seaf_db_trans_check_for_existence (trans,
        "SELECT resource_uid FROM cf_lock_lease WHERE resource_uid=? AND repo_id=? "
        "AND owner_user_id=? AND holder_id IS NOT NULL AND holder_id<>'' "
        "AND token_digest=? AND CAST(fencing AS CHAR)=? AND base_version=? "
        "AND expires_at>UTC_TIMESTAMP(6) FOR UPDATE", &error, 6,
        "string", uid, "string", repo, "string", user,
        "string", digest, "string", fencing, "string", base);
    return matches && !error ? 0 : -1;
}

static int local_commit_schema (SeafDBTrans *trans)
{
    gboolean error = FALSE;
    const char *checks[] = {
        "SELECT version FROM cf_schema_migration WHERE version='032_edit_commits' AND state='applied' AND step=1 FOR UPDATE",
        "SELECT commit_id FROM cf_edit_commit LIMIT 0 FOR UPDATE",
        "SELECT version FROM cf_schema_migration WHERE version='030_local_devices' AND state='applied' AND step=2 FOR UPDATE",
        "SELECT version FROM cf_schema_migration WHERE version='031_edit_sessions' AND state='applied' AND step=1 FOR UPDATE",
        "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('cf_edit_session','cf_local_device','cf_resource') AND ENGINE='InnoDB' HAVING COUNT(*)=3",
        "SELECT ENGINE FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_commit' AND ENGINE='InnoDB'",
        "SELECT COUNT(*) FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_commit' HAVING COUNT(*)=17 AND SUM((COLUMN_NAME IN ('commit_id','session_id','device_id','store_id') AND DATA_TYPE='char' AND CHARACTER_MAXIMUM_LENGTH=36 AND COLLATION_NAME='ascii_bin' AND IS_NULLABLE='NO') OR (COLUMN_NAME='provider' AND DATA_TYPE='varchar' AND CHARACTER_MAXIMUM_LENGTH=32 AND COLLATION_NAME='utf8mb4_bin' AND IS_NULLABLE='NO') OR (COLUMN_NAME='owner_user_id' AND DATA_TYPE='varchar' AND CHARACTER_MAXIMUM_LENGTH=225 AND COLLATION_NAME='utf8mb4_bin' AND IS_NULLABLE='NO') OR (COLUMN_NAME='snapshot' AND DATA_TYPE='longtext' AND COLLATION_NAME='utf8mb4_bin' AND IS_NULLABLE='NO') OR (COLUMN_NAME='upload_sha256' AND DATA_TYPE='char' AND CHARACTER_MAXIMUM_LENGTH=64 AND COLLATION_NAME='ascii_bin' AND IS_NULLABLE='NO') OR (COLUMN_NAME='state' AND DATA_TYPE='varchar' AND CHARACTER_MAXIMUM_LENGTH=12 AND COLLATION_NAME='ascii_bin' AND IS_NULLABLE='NO') OR (COLUMN_NAME IN ('new_file_id','published_head') AND DATA_TYPE='char' AND CHARACTER_MAXIMUM_LENGTH=40 AND COLLATION_NAME='ascii_bin' AND IS_NULLABLE='YES') OR (COLUMN_NAME IN ('device_revision','session_revision','upload_bytes','revision') AND DATA_TYPE='bigint' AND COLUMN_TYPE LIKE '%unsigned%' AND IS_NULLABLE='NO') OR (COLUMN_NAME IN ('created_at','updated_at') AND DATA_TYPE='datetime' AND DATETIME_PRECISION=6 AND IS_NULLABLE='NO'))=17",
        "SELECT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_commit' AND INDEX_NAME='PRIMARY' GROUP BY INDEX_NAME HAVING COUNT(*)=1 AND SUM(COLUMN_NAME='commit_id' AND SEQ_IN_INDEX=1 AND SUB_PART IS NULL AND NON_UNIQUE=0)=1",
        "SELECT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_commit' AND INDEX_NAME='session_commit' GROUP BY INDEX_NAME HAVING COUNT(*)=1 AND SUM(COLUMN_NAME='session_id' AND SEQ_IN_INDEX=1 AND SUB_PART IS NULL AND NON_UNIQUE=0)=1"
    };
    /* LIMIT 0 pins MDL but has no row, so it is not an existence predicate. */
    if (seaf_db_trans_query(trans, checks[1], 0) < 0) return -1;
    for (size_t index = 0; index < G_N_ELEMENTS(checks); ++index) {
        if (index == 1) continue;
        if (!seaf_db_trans_check_for_existence(trans, checks[index], &error, 0) || error) return -1;
    }
    return local_session_schema(trans);
}

typedef struct {
    const char *file_id;
    char *revision;
    gboolean absent, same;
} LocalIndexReceipt;

static gboolean local_index_receipt (SeafDBRow *row, void *data)
{
    LocalIndexReceipt *receipt = data;
    const char *file = seaf_db_row_get_column_text(row, 0);
    const char *revision = seaf_db_row_get_column_text(row, 1);
    receipt->absent = file == NULL;
    receipt->same = file && !strcmp(file, receipt->file_id);
    g_free(receipt->revision);
    receipt->revision = g_strdup(revision);
    return TRUE;
}

int cf_local_commit_record_index (SeafDBTrans *trans, const char *provider,
                                  const char *user, const char *commit_id,
                                  const char *store_id, const char *expected_revision,
                                  const char *measured_sha256, gint64 measured_bytes,
                                  const char *indexed_file_id, char **receipt_revision)
{
    if (!receipt_revision) return -1;
    *receipt_revision = NULL;
    if (!trans || !valid_id(provider, 32) || !valid_id(user, 225) ||
        !valid_id(commit_id, 36) || strlen(commit_id) != 36 ||
        !valid_id(store_id, 36) || strlen(store_id) != 36 ||
        !valid_id(expected_revision, 20) || expected_revision[0] == '0' ||
        strspn(expected_revision, "0123456789") != strlen(expected_revision) ||
        !measured_sha256 || strlen(measured_sha256) != 64 ||
        strspn(measured_sha256, "0123456789abcdef") != 64 || measured_bytes < 0 ||
        !indexed_file_id || strlen(indexed_file_id) != 40 ||
        strspn(indexed_file_id, "0123456789abcdef") != 40 || local_commit_schema(trans) < 0) return -1;
    errno = 0;
    guint64 expected = g_ascii_strtoull(expected_revision, NULL, 10);
    if (errno == ERANGE || expected == G_MAXUINT64) return -1;
    char next[32];
    g_snprintf(next, sizeof(next), "%" G_GUINT64_FORMAT, expected + 1);
    LocalIndexReceipt receipt = {indexed_file_id, NULL, FALSE, FALSE};
    /* Pin both identity sources and exact live session before changing receipt.
     * The caller separately establishes current native write/target authority. */
    int count = seaf_db_trans_foreach_selected_row(trans,
        "SELECT c.new_file_id,CAST(c.revision AS CHAR) FROM cf_edit_commit c "
        "JOIN cf_edit_session s ON s.session_id=c.session_id JOIN cf_local_device d ON d.device_id=c.device_id "
        "WHERE c.commit_id=? AND c.provider=? AND c.owner_user_id=? AND c.store_id=? "
        "AND c.state='committing' AND c.published_head IS NULL AND c.upload_sha256=? AND c.upload_bytes=? "
        "AND CAST(c.revision AS CHAR) IN (?,?) "
        "AND s.provider=c.provider AND s.owner_user_id=c.owner_user_id AND s.device_id=c.device_id "
        "AND s.device_revision=c.device_revision AND s.snapshot=c.snapshot AND s.state='committing' "
        "AND c.session_revision<18446744073709551615 AND s.revision=c.session_revision+1 "
        "AND s.expires_at>FLOOR(UNIX_TIMESTAMP()) "
        "AND d.provider=c.provider AND d.owner_user_id=c.owner_user_id AND d.state='active' "
        "AND d.revision=c.device_revision LIMIT 2 FOR UPDATE", local_index_receipt, &receipt, 8,
        "string", commit_id, "string", provider, "string", user, "string", store_id,
        "string", measured_sha256, "int64", measured_bytes, "string", expected_revision, "string", next);
    if (count != 1 || !receipt.revision || (!receipt.absent && !receipt.same) ||
        (receipt.absent && strcmp(receipt.revision, expected_revision))) {
        g_free(receipt.revision); return -1;
    }
    if (receipt.same) {
        *receipt_revision = receipt.revision;
        return 0;
    }
    g_free(receipt.revision);
    if (seaf_db_trans_query(trans,
        "UPDATE cf_edit_commit SET new_file_id=?,revision=revision+1,updated_at=UTC_TIMESTAMP(6) "
        "WHERE commit_id=? AND CAST(revision AS CHAR)=? AND state='committing' "
        "AND new_file_id IS NULL AND published_head IS NULL", 3,
        "string", indexed_file_id, "string", commit_id, "string", expected_revision) < 0) return -1;
    gboolean error = FALSE;
    if (!seaf_db_trans_check_for_existence(trans,
        "SELECT commit_id FROM cf_edit_commit WHERE commit_id=? AND new_file_id=? "
        "AND CAST(revision AS CHAR)=? AND state='committing' AND published_head IS NULL FOR UPDATE",
        &error, 3, "string", commit_id, "string", indexed_file_id, "string", next) || error) return -1;
    *receipt_revision = g_strdup(next);
    return 0;
}

static int local_intent_check (SeafDBTrans *trans, const char *repo,
                                 const char *path, const char *old_object_id,
                                 const char *new_object_id, json_t *root,
                                 json_t *current_subject, int ce_permission,
                                 gboolean require_indexed)
{
    json_t *proof = json_object_get(root, "local_commit");
    json_t *context = json_object_get(root, "context");
    const char *commit = json_identifier(proof, "commit_id", 36);
    const char *revision = json_identifier(proof, "commit_revision", 20);
    const char *provider = json_identifier(context, "provider", 32);
    const char *user = json_identifier(context, "userId", 225);
    if (!trans || !repo || !path || !old_object_id ||
        !json_is_object(proof) || json_object_size(proof) != 2 || !commit || strlen(commit) != 36 ||
        !revision || revision[0] == '0' || strspn(revision, "0123456789") != strlen(revision) ||
        !provider || !user || !current_subject ||
        json_object_get(root, "local_session") || json_object_get(root, "read_transfer_expires_at") ||
        strlen(old_object_id) != 40 || strspn(old_object_id, "0123456789abcdef") != 40 ||
        (require_indexed && (!new_object_id || strlen(new_object_id) != 40 ||
                            strspn(new_object_id, "0123456789abcdef") != 40)) ||
        local_commit_schema(trans) < 0) return -1;
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, path, -1);
    if (!hash || lease_resource_location(trans, repo, path, hash, NULL) < 0) {
        g_free(hash); return -1;
    }
    LocalSessionTarget target = {repo, path, old_object_id, FALSE, NULL};
    int count = seaf_db_trans_foreach_selected_row(trans,
        "SELECT c.snapshot,r.uid,r.lifecycle_ref,COALESCE(r.local_open_type,'') "
        "FROM cf_edit_commit c JOIN cf_edit_session s ON s.session_id=c.session_id "
        "JOIN cf_local_device d ON d.device_id=c.device_id "
        "JOIN cf_resource r ON r.repo_id=? AND r.path_hash=? AND r.path=? AND r.kind='file' AND r.state='active' "
        "WHERE c.commit_id=? AND c.provider=? AND c.owner_user_id=? AND c.store_id=? "
        "AND CAST(c.revision AS CHAR)=? AND c.state='committing' AND (?=0 OR c.new_file_id=?) AND c.published_head IS NULL "
        "AND c.upload_bytes<=9223372036854775807 AND c.upload_sha256 REGEXP '^[0-9a-f]{64}$' "
        "AND s.provider=c.provider AND s.owner_user_id=c.owner_user_id AND s.device_id=c.device_id "
        "AND s.device_revision=c.device_revision AND s.snapshot=c.snapshot AND s.state='committing' "
        "AND c.session_revision<18446744073709551615 AND s.revision=c.session_revision+1 "
        "AND s.expires_at>FLOOR(UNIX_TIMESTAMP()) "
        "AND d.provider=c.provider AND d.owner_user_id=c.owner_user_id AND d.state='active' AND d.revision=c.device_revision "
        "LIMIT 2 FOR UPDATE", local_session_target, &target, 10,
        "string", repo, "string", hash, "string", path, "string", commit,
        "string", provider, "string", user, "string", repo, "string", revision,
        "int", require_indexed ? 1 : 0, "string", new_object_id ? new_object_id : "");
    g_free(hash);
    const char *mode = json_identifier(target.snapshot, "mode", 32);
    int result = count == 1 && target.valid && mode && strcmp(mode, "view") &&
        local_session_lease(trans, repo, user, target.snapshot) == 0 &&
        (strcmp(mode, "optimistic-edit") || cf_policy_check_unleased_write(trans, repo, path) == 0) &&
        cf_policy_check_write(trans, repo, path, provider, user, current_subject, ce_permission) == 0 ? 0 : -1;
    if (target.snapshot) json_decref(target.snapshot);
    return result;
}

int cf_policy_check_local_commit (SeafDBTrans *trans, const char *repo,
                                 const char *path, const char *old_object_id,
                                 const char *new_object_id, json_t *root,
                                 json_t *current_subject, int ce_permission)
{
    return local_intent_check(trans, repo, path, old_object_id, new_object_id,
        root, current_subject, ce_permission, TRUE);
}

int cf_policy_check_local_index (SeafDBTrans *trans, const char *repo,
                                const char *path, const char *old_object_id,
                                const char *measured_sha256, gint64 measured_bytes,
                                json_t *root, json_t *current_subject, int ce_permission)
{
    if (!measured_sha256 || strlen(measured_sha256) != 64 ||
        strspn(measured_sha256, "0123456789abcdef") != 64 || measured_bytes < 0 ||
        local_intent_check(trans, repo, path, old_object_id, NULL,
            root, current_subject, ce_permission, FALSE) < 0) return -1;
    gboolean error = FALSE;
    return seaf_db_trans_check_for_existence(trans,
        "SELECT commit_id FROM cf_edit_commit WHERE commit_id=? AND CAST(revision AS CHAR)=? "
        "AND state='committing' AND upload_sha256=? AND upload_bytes=? AND published_head IS NULL FOR UPDATE",
        &error, 4, "string", json_identifier(json_object_get(root, "local_commit"), "commit_id", 36),
        "string", json_identifier(json_object_get(root, "local_commit"), "commit_revision", 20),
        "string", measured_sha256, "int64", measured_bytes) && !error ? 0 : -1;
}

static gboolean local_mutation_identity (SeafDBRow *row, void *data)
{
    json_t *event = data;
    const char *names[] = {"session_id", "device_id", "resource_uid", "occurred_at"};
    for (int index = 0; index < 4; ++index) {
        const char *value = seaf_db_row_get_column_text(row, index);
        if (!value || json_object_set_new(event, names[index], json_string(value)) < 0) return FALSE;
    }
    return TRUE;
}

static gboolean local_mutation_sequence (SeafDBRow *row, void *data)
{
    json_t *event = data;
    const char *value = seaf_db_row_get_column_text(row, 0);
    if (!value || !*value || value[0] == '0' || strspn(value, "0123456789") != strlen(value)) return FALSE;
    return json_object_set_new(event, "sequence", json_string(value)) == 0;
}

int cf_local_commit_append_fact (SeafDBTrans *trans, const char *repo, const char *path,
                                const char *old_file_id, const char *new_file_id,
                                json_t *root, json_t *current_subject, int ce_permission)
{
    if (cf_policy_check_local_commit(trans, repo, path, old_file_id, new_file_id,
                                    root, current_subject, ce_permission) < 0) return -1;
    const char *commit = json_identifier(json_object_get(root, "local_commit"), "commit_id", 36);
    const char *user = json_identifier(json_object_get(root, "context"), "userId", 225);
    if (!g_uuid_string_is_valid(commit) || strspn(commit, "0123456789abcdef-") != 36) return -1;
    gboolean error = FALSE;
    const char *pins[] = {
        "SELECT sequence FROM cf_event_outbox LIMIT 0 FOR UPDATE",
        "SELECT event_id FROM cf_audit_event LIMIT 0 FOR UPDATE"
    };
    for (size_t index = 0; index < G_N_ELEMENTS(pins); ++index)
        if (seaf_db_trans_query(trans, pins[index], 0) < 0) return -1;
    const char *checks[] = {
        "SELECT version FROM cf_schema_migration WHERE version='003_outbox' AND state='applied' AND step=1 FOR UPDATE",
        "SELECT version FROM cf_schema_migration WHERE version='004_audit' AND state='applied' FOR UPDATE",
        "SELECT COUNT(*) FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME IN ('cf_event_outbox','cf_audit_event') AND ENGINE='InnoDB' HAVING COUNT(*)=2",
        "SELECT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_event_outbox' AND INDEX_NAME='PRIMARY' GROUP BY INDEX_NAME HAVING COUNT(*)=1 AND SUM(COLUMN_NAME='event_id' AND SEQ_IN_INDEX=1 AND NON_UNIQUE=0 AND SUB_PART IS NULL)=1",
        "SELECT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_event_outbox' AND INDEX_NAME='outbox_sequence' GROUP BY INDEX_NAME HAVING COUNT(*)=1 AND SUM(COLUMN_NAME='sequence' AND SEQ_IN_INDEX=1 AND NON_UNIQUE=0 AND SUB_PART IS NULL)=1",
        "SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_event_outbox' AND COLUMN_NAME='sequence' AND DATA_TYPE='bigint' AND COLUMN_TYPE LIKE '%unsigned%' AND EXTRA LIKE '%auto_increment%' AND IS_NULLABLE='NO'",
        "SELECT INDEX_NAME FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_audit_event' AND INDEX_NAME='audit_event_identity' GROUP BY INDEX_NAME HAVING COUNT(*)=1 AND SUM(COLUMN_NAME='event_id' AND SEQ_IN_INDEX=1 AND NON_UNIQUE=0 AND SUB_PART IS NULL)=1"
    };
    for (size_t index = 0; index < G_N_ELEMENTS(checks); ++index)
        if (!seaf_db_trans_check_for_existence(trans, checks[index], &error, 0) || error) return -1;
    /* Preserve the established 004 LONGTEXT contract; never narrow existing
     * audit paths merely to accommodate the new local-edit writer. */
    if (!seaf_db_trans_check_for_existence(trans,
        "SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() "
        "AND TABLE_NAME='cf_audit_event' AND COLUMN_NAME='source_path' AND DATA_TYPE='longtext' "
        "AND COLLATION_NAME='utf8mb4_bin' AND IS_NULLABLE='YES' "
        "AND CHARACTER_MAXIMUM_LENGTH>=?", &error, 1, "int", (int)g_utf8_strlen(path, -1)) || error) return -1;
    if (seaf_db_trans_check_for_existence(trans,
        "SELECT event_id FROM cf_event_outbox WHERE event_id=? FOR UPDATE", &error,
        1, "string", commit) || error) return -1;
    json_t *event = json_object();
    char *payload = NULL;
    char *stream = g_strconcat("repo.", repo, NULL);
    int result = -1;
    if (!event || !stream) goto out;
    int count = seaf_db_trans_foreach_selected_row(trans,
        "SELECT session_id,device_id,JSON_UNQUOTE(JSON_EXTRACT(snapshot,'$.resource_uid')),"
        "DATE_FORMAT(UTC_TIMESTAMP(6),'%Y-%m-%dT%H:%i:%s.%fZ') FROM cf_edit_commit WHERE commit_id=? FOR UPDATE",
        local_mutation_identity, event, 1, "string", commit);
    if (count != 1 || json_object_size(event) != 4) goto out;
    const char *session = json_string_value(json_object_get(event, "session_id"));
    const char *device = json_string_value(json_object_get(event, "device_id"));
    const char *uid = json_string_value(json_object_get(event, "resource_uid"));
    if (!session || !device || !uid || !g_uuid_string_is_valid(session) || !g_uuid_string_is_valid(device) ||
        !g_uuid_string_is_valid(uid) || strspn(session, "0123456789abcdef-") != 36 ||
        strspn(device, "0123456789abcdef-") != 36 || strspn(uid, "0123456789abcdef-") != 36) goto out;
    const char *names[] = {"event_id", "request_id", "actor_user_id", "actor_kind", "source", "action",
                          "result", "repo_id", "path", "resource_kind", "content_version", "stream"};
    const char *values[] = {commit, commit, user, "user", "server", "file.updated", "succeeded",
                           repo, path, "file", new_file_id, stream};
    for (size_t index = 0; index < G_N_ELEMENTS(names); ++index)
        if (json_object_set_new(event, names[index], json_string(values[index])) < 0) goto out;
    if (json_object_set_new(event, "schema_version", json_integer(1)) < 0 ||
        json_object_set(event, "recorded_at", json_object_get(event, "occurred_at")) < 0) goto out;
    if (seaf_db_trans_query(trans,
        "INSERT INTO cf_event_outbox(event_id,stream,schema_version,payload,created_at,audit_state,"
        "resource_state,resource_next_at,search_state,search_next_at) "
        "VALUES(?,?,1,'{}',UTC_TIMESTAMP(6),'done','queued',UTC_TIMESTAMP(6),'queued',UTC_TIMESTAMP(6))",
        2, "string", commit, "string", stream) < 0) goto out;
    count = seaf_db_trans_foreach_selected_row(trans,
        "SELECT CAST(sequence AS CHAR) FROM cf_event_outbox WHERE event_id=? FOR UPDATE",
        local_mutation_sequence, event, 1, "string", commit);
    if (count != 1 || !json_is_string(json_object_get(event, "sequence"))) goto out;
    payload = json_dumps(event, JSON_COMPACT | JSON_SORT_KEYS);
    if (!payload || strlen(payload) > 65536) goto out;
    if (seaf_db_trans_query(trans, "UPDATE cf_event_outbox SET payload=? WHERE event_id=?",
        2, "string", payload, "string", commit) < 0) goto out;
    const char *occurred = json_string_value(json_object_get(event, "occurred_at"));
    if (seaf_db_trans_query(trans,
        "INSERT INTO cf_audit_event(repo_id,object_type,object_id,operation,operator,source,result,occurred_at,"
        "source_path,target_path,event_id,schema_version,recorded_at,request_id,actor_user_id,actor_kind,"
        "delegator,resource_uid,event_payload) "
        "VALUES(?,'file',?,'file.updated',?,'server','succeeded',STR_TO_DATE(?,'%Y-%m-%dT%H:%i:%s.%fZ'),"
        "?,NULL,?,1,STR_TO_DATE(?,'%Y-%m-%dT%H:%i:%s.%fZ'),?,?,'user',NULL,?,?)", 11,
        "string", repo, "string", uid, "string", user, "string", occurred, "string", path,
        "string", commit, "string", occurred, "string", commit, "string", user, "string", uid, "string", payload) < 0) goto out;
    result = 0;
out:
    if (event) json_decref(event);
    free(payload);
    g_free(stream);
    /* Caller must roll back every partial append on error. No commit here. */
    return result;
}

int cf_local_commit_finish (SeafDBTrans *trans, const char *repo, const char *path,
                           const char *old_file_id, const char *new_file_id,
                           const char *published_head, json_t *root,
                           json_t *current_subject, int ce_permission)
{
    if (!published_head || strlen(published_head) != 40 ||
        strspn(published_head, "0123456789abcdef") != 40 ||
        cf_policy_check_local_commit(trans, repo, path, old_file_id, new_file_id,
                                     root, current_subject, ce_permission) < 0) return -1;
    json_t *proof = json_object_get(root, "local_commit");
    json_t *context = json_object_get(root, "context");
    const char *commit = json_identifier(proof, "commit_id", 36);
    const char *revision = json_identifier(proof, "commit_revision", 20);
    const char *provider = json_identifier(context, "provider", 32);
    const char *user = json_identifier(context, "userId", 225);
    gboolean error = FALSE;
    const char *tables[] = {"cf_event_outbox", "cf_audit_event"};
    for (size_t index = 0; index < G_N_ELEMENTS(tables); ++index)
        if (!seaf_db_trans_check_for_existence(trans,
            "SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA=DATABASE() "
            "AND TABLE_NAME=? AND ENGINE='InnoDB'", &error, 1, "string", tables[index]) || error) return -1;
    /* A supplied 'audit succeeded' flag is never accepted. Pin the actual
     * matching durable fact and its audit row, plus the new actual Branch head.
     * The eventual fact writer must use commit_id as its immutable event ID. */
    if (!seaf_db_trans_check_for_existence(trans,
        "SELECT c.commit_id FROM cf_edit_commit c JOIN cf_edit_session s ON s.session_id=c.session_id "
        "JOIN Branch b ON b.repo_id=? AND b.name='master' AND b.commit_id=? "
        "JOIN cf_event_outbox e ON e.event_id=c.commit_id JOIN cf_audit_event a ON a.event_id=e.event_id "
        "WHERE c.commit_id=? AND c.provider=? AND c.owner_user_id=? AND c.state='committing' "
        "AND CAST(c.revision AS CHAR)=? AND c.new_file_id=? AND c.published_head IS NULL "
        "AND c.revision<18446744073709551615 AND s.revision<18446744073709551615 "
        "AND e.schema_version=1 AND e.audit_state='done' AND e.resource_state='queued' AND e.search_state='queued' "
        "AND e.stream=CONCAT('repo.',b.repo_id) AND JSON_VALID(e.payload) "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.event_id'))=c.commit_id "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.source'))='server' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.action'))='file.updated' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.result'))='succeeded' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.actor_kind'))='user' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.actor_user_id'))=c.owner_user_id "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.repo_id'))=b.repo_id "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.path'))=? "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.resource_kind'))='file' "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.resource_uid'))=JSON_UNQUOTE(JSON_EXTRACT(c.snapshot,'$.resource_uid')) "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.content_version'))=c.new_file_id "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.session_id'))=c.session_id "
        "AND JSON_UNQUOTE(JSON_EXTRACT(e.payload,'$.device_id'))=c.device_id "
        "AND a.schema_version=1 AND a.event_payload=e.payload AND a.repo_id=b.repo_id "
        "AND a.actor_user_id=c.owner_user_id AND a.actor_kind='user' AND a.source='server' "
        "AND a.operation='file.updated' AND a.result='succeeded' AND a.source_path=? "
        "AND a.resource_uid=JSON_UNQUOTE(JSON_EXTRACT(c.snapshot,'$.resource_uid')) FOR UPDATE",
        &error, 9, "string", repo, "string", published_head, "string", commit,
        "string", provider, "string", user, "string", revision, "string", new_file_id,
        "string", path, "string", path) || error) return -1;
    /* Event/audit waits may cross natural lease/session expiry even while all
     * mutation rows remain locked. Repeat real final intent/lease checks now. */
    if (cf_policy_check_local_commit(trans, repo, path, old_file_id, new_file_id,
                                    root, current_subject, ce_permission) < 0) return -1;
    if (seaf_db_trans_query(trans,
        "UPDATE cf_edit_session s JOIN cf_edit_commit c ON c.session_id=s.session_id "
        "SET s.state='completed',s.revision=s.revision+1,s.updated_at=UTC_TIMESTAMP(6) "
        "WHERE c.commit_id=? AND CAST(c.revision AS CHAR)=? AND c.state='committing' "
        "AND s.state='committing' AND s.revision=c.session_revision+1 "
        "AND s.expires_at>FLOOR(UNIX_TIMESTAMP())", 2, "string", commit, "string", revision) < 0) return -1;
    if (!seaf_db_trans_check_for_existence(trans,
        "SELECT s.session_id FROM cf_edit_session s JOIN cf_edit_commit c ON c.session_id=s.session_id "
        "WHERE c.commit_id=? AND s.state='completed' AND s.revision=c.session_revision+2 FOR UPDATE",
        &error, 1, "string", commit) || error) return -1;
    if (seaf_db_trans_query(trans,
        "UPDATE cf_edit_commit SET state='completed',published_head=?,revision=revision+1,updated_at=UTC_TIMESTAMP(6) "
        "WHERE commit_id=? AND CAST(revision AS CHAR)=? AND state='committing' AND published_head IS NULL",
        3, "string", published_head, "string", commit, "string", revision) < 0) return -1;
    if (!seaf_db_trans_check_for_existence(trans,
        "SELECT commit_id FROM cf_edit_commit WHERE commit_id=? AND state='completed' "
        "AND published_head=? AND new_file_id=? AND revision=CAST(? AS UNSIGNED)+1 FOR UPDATE",
        &error, 4, "string", commit, "string", published_head, "string", new_file_id, "string", revision) || error) return -1;
    /* No COMMIT here. Caller must roll back Branch, facts and both states if
     * any later publication guard/effect fails, including ambiguous I/O. */
    return 0;
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
        "AND state='applied' AND step=1 FOR UPDATE", &error, 0) || error) return -1;
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
