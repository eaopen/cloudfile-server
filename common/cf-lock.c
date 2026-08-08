/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * The lock truth deliberately lives in seafile-db: both seaf-server and the
 * Go fileserver consult this provider through the shared write lifecycle.
 * Seahub only asks these JSON adapters to create and release leases; it never
 * keeps an advisory copy that could be bypassed by sync or WebDAV.
 */

#include <jansson.h>
#include <string.h>
#include <time.h>

#include "common.h"
#include "cf-ext.h"
#include "cf-fileop.h"
#include "cf-lock.h"
#include "cf-path.h"
#include "log.h"
#include "seaf-db.h"
#include "seafile-error.h"
#include "seafile-session.h"

static gboolean lock_on = FALSE;

static int cf_lock_prepare (const CfFileOp *fop, GError **error);

typedef struct CfLockRow {
    char *lock_id;
    char *generation;
    char *owner;
    char *kind;
    gint64 lease_until;
    gint64 hard_expire_at;
    char *status;
} CfLockRow;

static void
lock_row_clear (CfLockRow *row)
{
    g_free (row->lock_id);
    g_free (row->generation);
    g_free (row->owner);
    g_free (row->kind);
    g_free (row->status);
    memset (row, 0, sizeof (*row));
}

static gboolean
load_lock_row_cb (SeafDBRow *db_row, void *data)
{
    CfLockRow *row = data;
    row->lock_id = g_strdup (seaf_db_row_get_column_text (db_row, 0));
    row->generation = g_strdup (seaf_db_row_get_column_text (db_row, 1));
    row->owner = g_strdup (seaf_db_row_get_column_text (db_row, 2));
    row->kind = g_strdup (seaf_db_row_get_column_text (db_row, 3));
    row->lease_until = seaf_db_row_get_column_int64 (db_row, 4);
    row->hard_expire_at = seaf_db_row_get_column_int64 (db_row, 5);
    row->status = g_strdup (seaf_db_row_get_column_text (db_row, 6));
    return FALSE;
}

static int
load_lock_row (const char *repo_id, const char *path, CfLockRow *row)
{
    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, path, -1);
    int ret = seaf_db_statement_foreach_row (
        seaf->db,
        "SELECT lock_id, generation, owner, kind, lease_until, hard_expire_at, status "
        "FROM cf_lock_lease WHERE repo_id=? AND path_hash=? AND normalized_path=?",
        load_lock_row_cb, row, 3, "string", repo_id, "string", path_hash,
        "string", path);
    g_free (path_hash);
    return ret;
}

static char *
like_descendant_pattern (const char *path)
{
    if (strcmp (path, "/") == 0)
        return g_strdup ("/%");
    GString *pattern = g_string_new ("");
    const char *ptr;
    for (ptr = path; *ptr; ptr++) {
        if (*ptr == '\\' || *ptr == '%' || *ptr == '_')
            g_string_append_c (pattern, '\\');
        g_string_append_c (pattern, *ptr);
    }
    g_string_append (pattern, "/%");
    return g_string_free (pattern, FALSE);
}

static int
load_descendant_lock_row (const char *repo_id, const char *path, CfLockRow *row)
{
    char *pattern = like_descendant_pattern (path);
    int ret = seaf_db_statement_foreach_row (
        seaf->db,
        "SELECT lock_id, generation, owner, kind, lease_until, hard_expire_at, status "
        "FROM cf_lock_lease WHERE repo_id=? AND normalized_path LIKE ? ESCAPE '\\\\' "
        "ORDER BY normalized_path LIMIT 1",
        load_lock_row_cb, row, 2, "string", repo_id, "string", pattern);
    g_free (pattern);
    return ret;
}

static gboolean
lock_is_live (const CfLockRow *row, gint64 now)
{
    return row->status && strcmp (row->status, "active") == 0 &&
           row->lease_until > now && row->hard_expire_at > now;
}

static const char *
request_string (json_t *obj, const char *name)
{
    json_t *value = json_object_get (obj, name);
    if (!value || !json_is_string (value))
        return NULL;
    const char *str = json_string_value (value);
    return str && *str ? str : NULL;
}

static gint64
json_seconds (json_t *obj, const char *name, gint64 fallback, gint64 maximum)
{
    json_t *value = json_object_get (obj, name);
    if (!value || !json_is_integer (value))
        return fallback;
    gint64 seconds = json_integer_value (value);
    if (seconds < 30)
        return fallback;
    return MIN (seconds, maximum);
}

static char *
dump_response (json_t *response)
{
    char *raw = json_dumps (response, JSON_COMPACT);
    char *out = g_strdup (raw ? raw : "{\"ok\":false,\"reason\":\"serialization_failed\"}");
    free (raw);
    json_decref (response);
    return out;
}

static char *
bad_request (GError **error, const char *message)
{
    g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS, "%s", message);
    return NULL;
}

static json_t *
parse_request (const char *request_json, GError **error)
{
    json_error_t json_error;
    json_t *request = json_loadb (request_json ? request_json : "", request_json ? strlen (request_json) : 0,
                                  0, &json_error);
    if (!request || !json_is_object (request)) {
        if (request)
            json_decref (request);
        bad_request (error, "Malformed CloudFile lock request");
        return NULL;
    }
    return request;
}

static gboolean
request_object (json_t *request, const char **repo_id, char **path, GError **error)
{
    *repo_id = request_string (request, "repo_id");
    const char *raw_path = request_string (request, "path");
    if (!*repo_id || !raw_path) {
        bad_request (error, "repo_id and path are required");
        return FALSE;
    }
    *path = cf_path_normalize (raw_path);
    if (strcmp (*path, "/") == 0) {
        g_free (*path);
        *path = NULL;
        bad_request (error, "A file path is required");
        return FALSE;
    }
    return TRUE;
}

gboolean
cf_lock_enabled (void)
{
    return lock_on;
}

void
cf_lock_init (void)
{
    char *backend = cf_ext_config_string ("lock_backend");
    lock_on = cf_ext_config_bool ("file_lock_enabled") &&
              (!backend || strcmp (backend, "cloudfile") == 0);
    if (backend && strcmp (backend, "cloudfile") != 0) {
        seaf_warning ("CloudFile: refusing non-CloudFile lock backend '%s' in CE.\n", backend);
        lock_on = FALSE;
    }
    g_free (backend);
    if (!lock_on)
        return;

    cf_fileop_register ("file-lock", cf_lock_prepare, NULL, NULL);
    seaf_message ("CloudFile: lease-backed file lock provider enabled.\n");
}

/* Return the first active lock on a changed object or beneath a changed
 * directory. Prefix matching is confined to write preparation, never reused
 * by read-side permission checks. */
static int
check_path (const char *repo_id, const char *path, const char *user, GError **error)
{
    CfLockRow row = {0};
    gint64 now = (gint64)time (NULL);
    int ret = load_lock_row (repo_id, path, &row);
    if (ret < 0) {
        seaf_warning ("CloudFile: cannot read file lock for %s:%s.\n", repo_id, path);
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL, "File lock service unavailable");
        return -1;
    }
    if (row.lock_id && lock_is_live (&row, now) &&
        (!user || !row.owner || strcmp (user, row.owner) != 0))
        goto locked;

    /* A directory operation must also respect a lock on any descendant. */
    lock_row_clear (&row);
    ret = load_descendant_lock_row (repo_id, path, &row);
    if (ret < 0) {
        seaf_warning ("CloudFile: cannot read descendant file locks for %s:%s.\n", repo_id, path);
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL, "File lock service unavailable");
        return -1;
    }
    if (!row.lock_id || !lock_is_live (&row, now) ||
        (user && row.owner && strcmp (user, row.owner) == 0)) {
        lock_row_clear (&row);
        return 0;
    }

locked:
    g_set_error (error, SEAFILE_DOMAIN, CF_ERR_FILE_LOCKED,
                 "File %s is locked by %s", path, row.owner ? row.owner : "another user");
    lock_row_clear (&row);
    return -1;
}

static int
cf_lock_prepare (const CfFileOp *fop, GError **error)
{
    if (!lock_on || cf_fileop_op_pathless (fop->op) || !fop->repo_id)
        return 0;
    GList *subjects = cf_fileop_subject_paths (fop);
    GList *sources = cf_fileop_source_paths (fop);
    GList *ptr;
    int ret = 0;
    for (ptr = subjects; ptr && ret == 0; ptr = ptr->next)
        ret = check_path (fop->repo_id, ptr->data, fop->user, error);
    const char *source_repo_id = fop->src_repo_id ? fop->src_repo_id : fop->repo_id;
    for (ptr = sources; ptr && ret == 0; ptr = ptr->next)
        ret = check_path (source_repo_id, ptr->data, fop->user, error);
    g_list_free_full (subjects, g_free);
    g_list_free_full (sources, g_free);
    return ret;
}

char *
cf_lock_status_json (const char *request_json, GError **error)
{
    json_t *request = parse_request (request_json, error);
    if (!request)
        return NULL;
    const char *repo_id;
    char *path = NULL;
    if (!request_object (request, &repo_id, &path, error)) {
        json_decref (request);
        return NULL;
    }
    CfLockRow row = {0};
    int ret = load_lock_row (repo_id, path, &row);
    json_t *response = json_object ();
    json_object_set_new (response, "ok", json_boolean (ret == 0));
    if (ret != 0) {
        json_object_set_new (response, "reason", json_string ("lock_service_unavailable"));
        json_object_set_new (response, "locked", json_false ());
    } else if (lock_is_live (&row, (gint64)time (NULL))) {
        json_object_set_new (response, "locked", json_true ());
        json_object_set_new (response, "owner", json_string (row.owner));
        json_object_set_new (response, "kind", json_string (row.kind));
        json_object_set_new (response, "generation", json_string (row.generation));
        json_object_set_new (response, "lease_until", json_integer (row.lease_until));
    } else {
        json_object_set_new (response, "locked", json_false ());
    }
    lock_row_clear (&row);
    g_free (path);
    json_decref (request);
    return dump_response (response);
}

char *
cf_lock_acquire_json (const char *request_json, GError **error)
{
    json_t *request = parse_request (request_json, error);
    if (!request)
        return NULL;
    const char *repo_id, *owner = request_string (request, "owner");
    const char *kind = request_string (request, "kind");
    char *path = NULL;
    if (!owner || !kind || !request_object (request, &repo_id, &path, error)) {
        g_free (path);
        json_decref (request);
        return bad_request (error, "owner, kind, repo_id and path are required");
    }
    if (strcmp (kind, "checkout") && strcmp (kind, "local-edit") &&
        strcmp (kind, "onlyoffice") && strcmp (kind, "pro-compatible")) {
        g_free (path); json_decref (request);
        return bad_request (error, "Unsupported lock kind");
    }

    gint64 now = (gint64)time (NULL);
    gint64 lease_seconds = json_seconds (request, "lease_seconds", 1800, 72 * 3600);
    gint64 hard_seconds = json_seconds (request, "hard_expire_seconds", 72 * 3600, 7 * 24 * 3600);
    char *lock_id = g_uuid_string_random ();
    char *generation = g_uuid_string_random ();
    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, path, -1);
    SeafDBTrans *trans = seaf_db_begin_transaction (seaf->db);
    if (!trans) {
        g_free (lock_id); g_free (generation); g_free (path_hash); g_free (path); json_decref (request);
        return bad_request (error, "File lock service unavailable");
    }
    int ret = seaf_db_trans_query (trans,
        "INSERT INTO cf_lock_lease (repo_id, normalized_path, path_hash, lock_id, generation, owner, kind, status, lease_until, hard_expire_at, created_at, updated_at) "
        "VALUES (?, ?, ?, '', '', '', '', 'released', 0, 0, ?, ?) "
        "ON DUPLICATE KEY UPDATE repo_id=VALUES(repo_id)",
        5, "string", repo_id, "string", path, "string", path_hash, "int64", now, "int64", now);
    CfLockRow row = {0};
    if (ret == 0)
        ret = seaf_db_trans_foreach_selected_row (trans,
            "SELECT lock_id, generation, owner, kind, lease_until, hard_expire_at, status "
            "FROM cf_lock_lease WHERE repo_id=? AND path_hash=? AND normalized_path=? FOR UPDATE",
            load_lock_row_cb, &row, 3, "string", repo_id, "string", path_hash, "string", path);
    gboolean conflict = ret == 0 && lock_is_live (&row, now);
    if (ret == 0 && !conflict)
        ret = seaf_db_trans_query (trans,
            "UPDATE cf_lock_lease SET lock_id=?, generation=?, owner=?, kind=?, status='active', lease_until=?, hard_expire_at=?, last_heartbeat_at=?, updated_at=? WHERE repo_id=? AND path_hash=? AND normalized_path=?",
            11, "string", lock_id, "string", generation, "string", owner, "string", kind,
            "int64", now + lease_seconds, "int64", now + hard_seconds, "int64", now, "int64", now,
            "string", repo_id, "string", path_hash, "string", path);
    if (ret == 0 && !conflict)
        ret = seaf_db_trans_query (trans,
            "INSERT INTO cf_lock_repo_revision (repo_id, revision, updated_at) VALUES (?, 1, ?) "
            "ON DUPLICATE KEY UPDATE revision=revision+1, updated_at=VALUES(updated_at)",
            2, "string", repo_id, "int64", now);
    if (ret == 0 && !conflict)
        ret = seaf_db_commit (trans);
    else
        seaf_db_rollback (trans);
    seaf_db_trans_close (trans);

    json_t *response = json_object ();
    if (ret != 0) {
        json_object_set_new (response, "ok", json_false ());
        json_object_set_new (response, "reason", json_string ("lock_service_unavailable"));
    } else if (conflict) {
        json_object_set_new (response, "ok", json_false ());
        json_object_set_new (response, "reason", json_string ("locked"));
        json_object_set_new (response, "owner", json_string (row.owner ? row.owner : ""));
        json_object_set_new (response, "lease_until", json_integer (row.lease_until));
    } else {
        json_object_set_new (response, "ok", json_true ());
        json_object_set_new (response, "lock_id", json_string (lock_id));
        json_object_set_new (response, "generation", json_string (generation));
        json_object_set_new (response, "lease_until", json_integer (now + lease_seconds));
    }
    lock_row_clear (&row);
    g_free (lock_id); g_free (generation); g_free (path_hash); g_free (path); json_decref (request);
    return dump_response (response);
}

char *
cf_lock_refresh_json (const char *request_json, GError **error)
{
    json_t *request = parse_request (request_json, error);
    if (!request)
        return NULL;
    const char *repo_id, *owner = request_string (request, "owner");
    const char *generation = request_string (request, "generation");
    char *path = NULL;
    if (!owner || !generation || !request_object (request, &repo_id, &path, error)) {
        g_free (path); json_decref (request);
        return bad_request (error, "owner, generation, repo_id and path are required");
    }

    gint64 now = (gint64)time (NULL);
    gint64 lease_seconds = json_seconds (request, "lease_seconds", 12 * 60 * 60, 72 * 3600);
    SeafDBTrans *trans = seaf_db_begin_transaction (seaf->db);
    if (!trans) {
        g_free (path); json_decref (request);
        return bad_request (error, "File lock service unavailable");
    }

    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, path, -1);
    CfLockRow row = {0};
    int ret = seaf_db_trans_foreach_selected_row (trans,
        "SELECT lock_id, generation, owner, kind, lease_until, hard_expire_at, status "
        "FROM cf_lock_lease WHERE repo_id=? AND path_hash=? AND normalized_path=? FOR UPDATE",
        load_lock_row_cb, &row, 3, "string", repo_id, "string", path_hash, "string", path);
    gboolean valid = ret == 0 && lock_is_live (&row, now) &&
        row.owner && strcmp (row.owner, owner) == 0 &&
        row.generation && strcmp (row.generation, generation) == 0;
    gint64 lease_until = valid ? MIN (now + lease_seconds, row.hard_expire_at) : 0;
    if (valid)
        ret = seaf_db_trans_query (trans,
            "UPDATE cf_lock_lease SET lease_until=?, last_heartbeat_at=?, updated_at=? "
            "WHERE repo_id=? AND path_hash=? AND normalized_path=? AND owner=? AND generation=? AND status='active'",
            9, "int64", lease_until, "int64", now, "int64", now,
            "string", repo_id, "string", path_hash, "string", path,
            "string", owner, "string", generation);
    if (ret == 0 && valid)
        ret = seaf_db_commit (trans);
    else
        seaf_db_rollback (trans);
    seaf_db_trans_close (trans);

    json_t *response = json_object ();
    if (ret != 0) {
        json_object_set_new (response, "ok", json_false ());
        json_object_set_new (response, "reason", json_string ("lock_service_unavailable"));
    } else if (!valid) {
        json_object_set_new (response, "ok", json_false ());
        json_object_set_new (response, "reason", json_string ("not_owner_or_stale"));
    } else {
        json_object_set_new (response, "ok", json_true ());
        json_object_set_new (response, "generation", json_string (generation));
        json_object_set_new (response, "lease_until", json_integer (lease_until));
    }
    lock_row_clear (&row);
    g_free (path_hash); g_free (path); json_decref (request);
    return dump_response (response);
}

char *
cf_lock_release_json (const char *request_json, GError **error)
{
    json_t *request = parse_request (request_json, error);
    if (!request)
        return NULL;
    const char *repo_id, *owner = request_string (request, "owner");
    const char *generation = request_string (request, "generation");
    char *path = NULL;
    if (!owner || !request_object (request, &repo_id, &path, error)) {
        g_free (path); json_decref (request);
        return bad_request (error, "owner, repo_id and path are required");
    }
    gint64 now = (gint64)time (NULL);
    CfLockRow current = {0};
    int current_ret = load_lock_row (repo_id, path, &current);
    if (current_ret != 0 || !lock_is_live (&current, now) ||
        !current.owner || strcmp (current.owner, owner) != 0 ||
        (generation && (!current.generation || strcmp (current.generation, generation) != 0))) {
        json_t *response = json_object ();
        json_object_set_new (response, "ok", json_false ());
        json_object_set_new (response, "reason", json_string (
            current_ret != 0 ? "lock_service_unavailable" : "not_owner_or_stale"));
        lock_row_clear (&current);
        g_free (path); json_decref (request);
        return dump_response (response);
    }
    lock_row_clear (&current);
    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, path, -1);
    const char *sql = generation
        ? "UPDATE cf_lock_lease SET status='released', updated_at=? WHERE repo_id=? AND path_hash=? AND normalized_path=? AND owner=? AND generation=? AND status='active'"
        : "UPDATE cf_lock_lease SET status='released', updated_at=? WHERE repo_id=? AND path_hash=? AND normalized_path=? AND owner=? AND status='active'";
    int n = generation ? 6 : 5;
    int ret = generation
        ? seaf_db_statement_query (seaf->db, sql, n, "int64", now, "string", repo_id, "string", path_hash, "string", path, "string", owner, "string", generation)
        : seaf_db_statement_query (seaf->db, sql, n, "int64", now, "string", repo_id, "string", path_hash, "string", path, "string", owner);
    json_t *response = json_object ();
    json_object_set_new (response, "ok", json_boolean (ret == 0));
    if (ret == 0)
        seaf_db_statement_query (seaf->db,
            "INSERT INTO cf_lock_repo_revision (repo_id, revision, updated_at) VALUES (?, 1, ?) ON DUPLICATE KEY UPDATE revision=revision+1, updated_at=VALUES(updated_at)",
            2, "string", repo_id, "int64", now);
    g_free (path_hash); g_free (path); json_decref (request);
    return dump_response (response);
}

char *
cf_lock_force_release_json (const char *request_json, GError **error)
{
    json_t *request = parse_request (request_json, error);
    if (!request)
        return NULL;
    const char *repo_id, *actor = request_string (request, "actor");
    const char *generation = request_string (request, "generation");
    const char *reason = request_string (request, "reason");
    char *path = NULL;
    if (!actor || !generation || !request_object (request, &repo_id, &path, error)) {
        g_free (path); json_decref (request);
        return bad_request (error, "actor, generation, repo_id and path are required");
    }
    if (!reason)
        reason = "administrator_force_release";

    gint64 now = (gint64)time (NULL);
    SeafDBTrans *trans = seaf_db_begin_transaction (seaf->db);
    if (!trans) {
        g_free (path); json_decref (request);
        return bad_request (error, "File lock service unavailable");
    }

    char *path_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA1, path, -1);
    CfLockRow row = {0};
    int ret = seaf_db_trans_foreach_selected_row (trans,
        "SELECT lock_id, generation, owner, kind, lease_until, hard_expire_at, status "
        "FROM cf_lock_lease WHERE repo_id=? AND path_hash=? AND normalized_path=? FOR UPDATE",
        load_lock_row_cb, &row, 3, "string", repo_id, "string", path_hash, "string", path);
    gboolean valid = ret == 0 && lock_is_live (&row, now) &&
        row.generation && strcmp (row.generation, generation) == 0;
    if (valid)
        ret = seaf_db_trans_query (trans,
            "UPDATE cf_lock_lease SET status='released', forced_by=?, forced_reason=?, updated_at=? "
            "WHERE repo_id=? AND path_hash=? AND normalized_path=? AND generation=? AND status='active'",
            8, "string", actor, "string", reason, "int64", now,
            "string", repo_id, "string", path_hash, "string", path, "string", generation);
    if (ret == 0 && valid)
        ret = seaf_db_trans_query (trans,
            "INSERT INTO cf_lock_repo_revision (repo_id, revision, updated_at) VALUES (?, 1, ?) "
            "ON DUPLICATE KEY UPDATE revision=revision+1, updated_at=VALUES(updated_at)",
            2, "string", repo_id, "int64", now);
    if (ret == 0 && valid)
        ret = seaf_db_commit (trans);
    else
        seaf_db_rollback (trans);
    seaf_db_trans_close (trans);

    json_t *response = json_object ();
    if (ret != 0) {
        json_object_set_new (response, "ok", json_false ());
        json_object_set_new (response, "reason", json_string ("lock_service_unavailable"));
    } else if (!valid) {
        json_object_set_new (response, "ok", json_false ());
        json_object_set_new (response, "reason", json_string ("not_found_or_stale"));
    } else {
        json_object_set_new (response, "ok", json_true ());
    }
    lock_row_clear (&row);
    g_free (path_hash); g_free (path); json_decref (request);
    return dump_response (response);
}
