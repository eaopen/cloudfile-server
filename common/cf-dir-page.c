/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#include "common.h"
#include <jansson.h>

#include "cf-dir-page.h"
#include "cf-ext.h"
#include "repo-mgr.h"
#include "seafile-session.h"
#include "seafile-rpc.h"
#include "seafile-error.h"
#include "utils.h"

#define DEBUG_FLAG SEAFILE_DEBUG_OTHER
#include "log.h"

/* Preserve the CE Dirent fields used by the existing Hub presentation code;
 * JSON is an envelope because objlist RPCs cannot carry scan metadata. */
static json_t *
dirent_to_json (GObject *entry)
{
    char *id = NULL, *name = NULL, *modifier = NULL, *permission = NULL;
    char *lock_owner = NULL;
    int mode, version;
    gint64 mtime, size, lock_time;
    gboolean is_locked, is_shared;
    g_object_get (entry, "obj_id", &id, "obj_name", &name,
                  "mode", &mode, "version", &version, "mtime", &mtime,
                  "size", &size, "modifier", &modifier,
                  "permission", &permission, "is_locked", &is_locked,
                  "lock_owner", &lock_owner, "lock_time", &lock_time,
                  "is_shared", &is_shared, NULL);
    json_t *result = json_pack (
        "{s:s,s:s,s:i,s:i,s:I,s:I,s:s?,s:s?,s:b,s:s?,s:I,s:b}",
        "obj_id", id, "obj_name", name, "mode", mode, "version", version,
        "mtime", (json_int_t)mtime, "size", (json_int_t)size,
        "modifier", modifier, "permission", permission,
        "is_locked", is_locked, "lock_owner", lock_owner,
        "lock_time", (json_int_t)lock_time, "is_shared", is_shared);
    g_free (id);
    g_free (name);
    g_free (modifier);
    g_free (permission);
    g_free (lock_owner);
    return result;
}

char *
cf_list_dir_page_json (const char *request_json, GError **error)
{
    json_t *request = request_json ? json_loads (request_json, 0, NULL) : NULL;
    json_t *response = NULL, *items = NULL;
    char *result = NULL, *path = NULL, *revision = NULL;
    char *native_perm = NULL, *parent_perm = NULL;
    GList *entries = NULL, *ptr;
    CfDirScan scan = {0};

    const char *repo_id = json_string_value (json_object_get (request, "repo_id"));
    const char *raw_path = json_string_value (json_object_get (request, "path"));
    const char *user = json_string_value (json_object_get (request, "user"));
    const char *expected = json_string_value (json_object_get (request, "dir_revision"));
    json_t *start_value = json_object_get (request, "start");
    json_t *limit_value = json_object_get (request, "limit");
    json_int_t start = json_integer_value (start_value);
    json_int_t limit = json_integer_value (limit_value);

    if (!json_is_object (request) || !repo_id || !is_uuid_valid (repo_id) ||
        !raw_path || raw_path[0] != '/' || strlen (raw_path) > 4096 ||
        !user || !user[0] || !expected || !is_object_id_valid (expected) ||
        !json_is_integer (start_value) || !json_is_integer (limit_value) ||
        limit < 1 || limit > 500 || start < 0 || start > G_MAXINT - limit) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_BAD_ARGS,
                     "Invalid directory page request");
        goto out;
    }
    path = format_dir_path (raw_path);

    /* Revision pins content, never authorization. Recheck the current parent
     * and keep exactly the existing per-entry capability filter below. */
    native_perm = seaf_repo_manager_check_permission (seaf->repo_mgr, repo_id, user, error);
    if (!native_perm) {
        if (!error || !*error)
            g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_REPO_AUTH, "Access denied");
        goto out;
    }
    parent_perm = cf_ext_check_permission (repo_id, path, user, native_perm);
    if (!parent_perm) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_REPO_AUTH, "Access denied");
        goto out;
    }

    /* This resolves one current head; the immutable ID is then used for the
     * read as well. Do not read an arbitrary historical ID supplied by Hub. */
    revision = seafile_get_dir_id_by_path (repo_id, path, error);
    if (!revision) {
        if (!error || !*error)
            g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_DIR_MISSING, "Folder not found");
        goto out;
    }
    if (strcmp (revision, expected) != 0) {
        /* A machine-readable result avoids matching translated RPC errors. */
        response = json_pack ("{s:s}", "error", "DIR_REVISION_CHANGED");
        goto serialize;
    }

    entries = seaf_repo_manager_list_dir_with_perm_page (
        seaf->repo_mgr, repo_id, path, revision, user, (int)start, (int)limit, &scan, error);
    if (error && *error)
        goto out;
    entries = cf_ext_filter_dirents (repo_id, path, user, entries);

    items = json_array ();
    for (ptr = entries; ptr; ptr = ptr->next) {
        json_t *entry = dirent_to_json (ptr->data);
        if (!entry || json_array_append_new (items, entry) < 0) {
            g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_INTERNAL, "Invalid directory entry");
            goto out;
        }
    }
    response = json_pack ("{s:s,s:i,s:b,s:i}",
                          "dir_revision", revision, "scanned_count", scan.scanned_count,
                          "scan_exhausted", scan.scan_exhausted,
                          "visible_count", (int)json_array_size (items));
    json_object_set (response, "visible_items", items);
    json_object_set_new (response, "next_scan_position", scan.scan_exhausted ?
                         json_null () : json_integer (start + scan.scanned_count));
    /* Numeric diagnostics exclude paths and hidden names. This does not imply
     * pageSize-bounded directory read or sort cost. */
    seaf_debug ("CloudFile directory page: scanned_count=%d visible_count=%d "
                "next_scan_position=%" G_GINT64_FORMAT " scan_exhausted=%d\n",
                scan.scanned_count, (int)json_array_size (items),
                scan.scan_exhausted ? (gint64)-1 : (gint64)(start + scan.scanned_count),
                scan.scan_exhausted);

serialize:
    result = json_dumps (response, JSON_COMPACT);
out:
    json_decref (items);
    json_decref (response);
    json_decref (request);
    g_list_free_full (entries, g_object_unref);
    g_free (path);
    g_free (revision);
    g_free (native_perm);
    g_free (parent_perm);
    return result;
}
