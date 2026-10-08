/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * CloudFile storage-class assignment (P2 storage backends).
 *
 * The storage backends themselves (obj/block/fs routing, S3 client, migration)
 * live in the upstream storage files; this file only exposes the two small
 * pieces the Seahub self-service API needs: list the configured classes, and
 * pin a repo to one of them.  Both are gated by [cloudfile] s3_storage_enabled
 * so that with every switch off the RPC surface behaves exactly like CE (the
 * wrapped callers return "disabled" before touching anything).
 */

#include "common.h"

#include <jansson.h>
#include <string.h>

#include "cf-ext.h"
#include "cf-auto-storage.h"
#include "cf-storage.h"
#include "log.h"
#include "repo-mgr.h"
#include "seaf-db.h"
#include "seafile-error.h"
#include "seafile-session.h"

gboolean
cf_storage_enabled (void)
{
    return cf_ext_config_bool ("s3_storage_enabled");
}

int
cf_set_repo_storage_id (const char *repo_id, const char *storage_id)
{
    const char *key = cf_auto_storage_key (storage_id);
    gboolean db_err = FALSE;
    SeafDBTrans *trans;
    if (g_str_has_prefix (storage_id, CF_AUTO_STORAGE_PREFIX) && !key)
        return -1;
    trans = seaf_db_begin_transaction (seaf->db);

    if (!trans)
        return -1;
    /* Unique key reservations are part of the same transaction as the pin.
     * Keep them after permanent deletion so a cold GC can locate this root. */
    if (key) {
        gboolean reserved = seaf_db_trans_check_for_existence (trans,
            "SELECT 1 FROM cf_library_storage_key WHERE repo_key = ? AND repo_id = ?",
            &db_err, 2, "string", key, "string", repo_id);
        if (db_err || (!reserved && seaf_db_trans_query (trans,
                "INSERT INTO cf_library_storage_key (repo_key, repo_id) VALUES (?, ?)",
                2, "string", key, "string", repo_id) < 0)) {
            seaf_db_rollback (trans);
            seaf_db_trans_close (trans);
            return -1;
        }
    }
    if (seaf_db_trans_query (
            trans, "DELETE FROM RepoStorageId WHERE repo_id = ?", 1,
            "string", repo_id) < 0 ||
        seaf_db_trans_query (
            trans, "INSERT INTO RepoStorageId (repo_id, storage_id) "
            "VALUES (?, ?)", 2, "string", repo_id,
            "string", storage_id) < 0 ||
        seaf_db_commit (trans) < 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -1;
    }
    seaf_db_trans_close (trans);
    return 0;
}

int
cf_pin_default_local_storage (const char *repo_id)
{
    gboolean db_err = FALSE;
    gboolean assigned;
    char *raw, *storage_id;
    json_t *classes;
    size_t index;
    gboolean enabled = FALSE;
    int ret;
    if (!cf_storage_enabled () || !g_key_file_get_boolean (seaf->config,
            "storage", "enable_storage_classes", NULL))
        return 0;
    raw = cf_get_storage_classes_json (NULL);
    if (!raw)
        return -1;
    classes = json_loads (raw, 0, NULL);
    g_free (raw);
    if (!classes)
        return -1;
    for (index = 0; index < json_array_size (classes); ++index)
        if (json_is_true (json_object_get (json_array_get (classes, index), "is_auto")))
            enabled = TRUE;
    json_decref (classes);
    if (!enabled)
        return 0;
    assigned = seaf_db_statement_exists (seaf->db,
        "SELECT 1 FROM RepoStorageId WHERE repo_id = ?", &db_err,
        1, "string", repo_id);
    if (db_err)
        return -1;
    if (assigned)
        return 0;
    storage_id = g_strconcat (CF_AUTO_STORAGE_PREFIX, repo_id, NULL);
    ret = cf_set_repo_storage_id (repo_id, storage_id);
    g_free (storage_id);
    return ret;
}

char *
cf_create_repo_json (const char *request_json, GError **error)
{
    json_t *req;
    json_error_t json_error;
    const char *name;
    const char *desc;
    const char *owner;
    const char *passwd;
    const char *storage_id;
    const char *repo_key;
    char *auto_storage_id = NULL;
    const char *pwd_hash_algo;
    const char *pwd_hash_params;
    int enc_version;
    char *repo_id;

    req = json_loads (request_json, 0, &json_error);
    if (!req) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS,
                     "Invalid request JSON at line %d: %s",
                     json_error.line, json_error.text);
        return NULL;
    }

    name = json_string_value (json_object_get (req, "name"));
    owner = json_string_value (json_object_get (req, "owner"));
    if (!name || !name[0] || !owner || !owner[0]) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS,
                     "name and owner are required.");
        json_decref (req);
        return NULL;
    }

    desc = json_string_value (json_object_get (req, "desc"));
    passwd = json_string_value (json_object_get (req, "passwd"));
    storage_id = json_string_value (json_object_get (req, "storage_id"));
    repo_key = json_string_value (json_object_get (req, "repo_key"));
    if (repo_key) {
        if (!cf_auto_key_valid (repo_key) || (storage_id && storage_id[0])) {
            g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS,
                         "Invalid repo_key or conflicting storage_id.");
            json_decref (req);
            return NULL;
        }
        auto_storage_id = g_strconcat (CF_AUTO_STORAGE_PREFIX, repo_key, NULL);
        storage_id = auto_storage_id;
    }
    pwd_hash_algo = json_string_value (json_object_get (req, "pwd_hash_algo"));
    pwd_hash_params = json_string_value (json_object_get (req,
                                                          "pwd_hash_params"));
    if (json_object_get (req, "enc_version")) {
        enc_version = json_integer_value (json_object_get (req, "enc_version"));
    } else {
        enc_version = 2;
    }

    repo_id = seaf_repo_manager_create_new_repo (
        seaf->repo_mgr, name, desc ? desc : "", owner,
        passwd && passwd[0] ? passwd : NULL,
        enc_version, pwd_hash_algo, pwd_hash_params,
        storage_id && storage_id[0] ? storage_id : NULL,
        error);
    g_free (auto_storage_id);
    json_decref (req);
    return repo_id;
}

/* Resolve [storage] storage_classes_file the same way the multi-storage
 * backend does: an absolute path is used verbatim, a relative one is resolved
 * against <seafile-data>/../conf/ for traditional installations. */
static char *
resolve_storage_classes_path (void)
{
    char *path = g_key_file_get_string (seaf->config, "storage",
                                        "storage_classes_file", NULL);
    char *resolved;
    char *top_dir;

    if (!path || !path[0])
        return path;
    if (g_path_is_absolute (path))
        return path;

    top_dir = g_path_get_dirname (seaf->seaf_dir);
    resolved = g_build_filename (top_dir, "conf", path, NULL);
    g_free (top_dir);
    g_free (path);
    return resolved;
}

char *
cf_get_storage_classes_json (GError **error)
{
    json_t *classes;
    json_t *out;
    json_error_t json_error;
    char *path;
    size_t index;
    char *result = NULL;

    if (!g_key_file_get_boolean (seaf->config, "storage",
                                 "enable_storage_classes", NULL)) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Storage classes are not enabled.");
        return NULL;
    }

    path = resolve_storage_classes_path ();
    if (!path || !path[0]) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "storage_classes_file is not configured.");
        g_free (path);
        return NULL;
    }

    classes = json_load_file (path, 0, &json_error);
    g_free (path);
    if (!classes) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Failed to parse storage classes at line %d: %s",
                     json_error.line, json_error.text);
        return NULL;
    }
    if (!json_is_array (classes)) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Storage classes must be a JSON array.");
        json_decref (classes);
        return NULL;
    }

    out = json_array ();
    for (index = 0; index < json_array_size (classes); ++index) {
        json_t *item = json_array_get (classes, index);
        const char *storage_id;
        const char *storage_name;
        json_t *entry;

        if (!json_is_object (item))
            continue;
        storage_id = json_string_value (json_object_get (item, "storage_id"));
        if (!storage_id || !storage_id[0])
            continue;
        storage_name = json_string_value (
            json_object_get (item, "storage_name"));
        if (!storage_name || !storage_name[0])
            storage_name = storage_id;

        entry = json_pack ("{s:s,s:s,s:b}",
                           "storage_id", storage_id,
                           "storage_name", storage_name,
                           "is_default",
                           json_is_true (json_object_get (item,
                                                          "is_default")));
        if (entry) {
            /* Expose allocation capability, never host paths or credentials. */
            if (json_is_true (json_object_get (item, "is_auto")))
                json_object_set_new (entry, "is_auto", json_true ());
            json_array_append_new (out, entry);
        }
    }
    json_decref (classes);

    result = json_dumps (out, JSON_COMPACT);
    json_decref (out);
    if (!result)
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Failed to serialize storage classes.");
    return result;
}
