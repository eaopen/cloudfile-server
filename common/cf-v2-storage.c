/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* Minimal local routing; CE RepoStorageId remains the only route authority. */
#include "common.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include "seafile-session.h"
#include "seaf-db.h"
#include "utils.h"
#include "log.h"
#include "cf-v2-storage.h"

#define AUTO_PREFIX "auto-local:"

gboolean
cf_v2_storage_enabled (void)
{
    return seaf && seaf->config &&
        g_key_file_has_key (seaf->config, "cloudfile_v2", "auto_local_root", NULL);
}

static gboolean
real_directory (const char *path)
{
    struct stat st;
    return path && g_lstat (path, &st) == 0 && S_ISDIR (st.st_mode);
}

/* Deployment owns the root and its ancestors; never create a missing mount. */
static char *
storage_root (void)
{
    char *root = g_key_file_get_string (seaf->config, "cloudfile_v2",
                                      "auto_local_root", NULL);
    if (!root || !g_path_is_absolute (root) || !real_directory (root)) {
        seaf_warning ("V2 auto_local_root must be an existing absolute directory.\n");
        g_free (root);
        return NULL;
    }
    return root;
}

static gboolean
key_valid (const char *key)
{
    size_t i, len = key ? strlen (key) : 0;
    if (!len || len > 80)
        return FALSE;
    for (i = 0; i < len; ++i)
        if (!g_ascii_islower (key[i]) && !g_ascii_isdigit (key[i]) &&
            !(key[i] == '-' && i > 0 && i + 1 < len))
            return FALSE;
    return TRUE;
}

typedef struct {
    char *storage_id;
    char *key;
    char *state;
} Route;

static gboolean
read_route (SeafDBRow *row, void *data)
{
    Route *route = data;
    route->storage_id = g_strdup (seaf_db_row_get_column_text (row, 0));
    route->key = g_strdup (seaf_db_row_get_column_text (row, 1));
    route->state = g_strdup (seaf_db_row_get_column_text (row, 2));
    return TRUE;
}

char *
cf_v2_store_dir (const char *native_obj_dir, const char *repo_id)
{
    Route route = {0};
    char *root = NULL, *key_dir = NULL, *obj_type = NULL, *result = NULL;
    /* Preserve native offline export. Automatic-root discovery for export is
     * a separate follow-up; never add a SQL prerequisite to native recovery. */
    if (!cf_v2_storage_enabled ())
        return g_build_filename (native_obj_dir, repo_id, NULL);
    /* A configured managed root must never revert to native storage when
     * database initialization fails: reads and writes would diverge. */
    if (!seaf->db || !repo_id || !is_uuid_valid (repo_id) ||
        !(root = storage_root ()))
        return NULL;
    /* One query distinguishes unmapped native libraries from corrupt routes.
     * No cache: separate C/Go processes must see the same committed binding. */
    if (seaf_db_statement_foreach_row (seaf->db,
            "SELECT s.storage_id,c.repo_key,c.state FROM (SELECT ? AS repo_id) i "
            "LEFT JOIN RepoStorageId s ON s.repo_id=i.repo_id "
            "LEFT JOIN cf_library_storage c ON c.repo_id=i.repo_id",
            read_route, &route, 1, "string", repo_id) != 1)
        goto out;
    /* Registered pre-V2 native libraries retain their original object tree.
     * "native-<UUID>" is a reserved metadata marker, never a new storage
     * destination. Do not silently interpret a broken V2 route as native. */
    if (!route.storage_id && (!route.key ||
        ((g_strcmp0 (route.state, "active") == 0 ||
          g_strcmp0 (route.state, "retired") == 0) &&
         g_str_has_prefix (route.key, "native-") &&
         strcmp (route.key + strlen ("native-"), repo_id) == 0))) {
        result = g_build_filename (native_obj_dir, repo_id, NULL);
        goto out;
    }
    if (!route.storage_id || !key_valid (route.key) ||
        !g_str_has_prefix (route.storage_id, AUTO_PREFIX) ||
        strcmp (route.storage_id + strlen (AUTO_PREFIX), route.key) != 0 ||
        (g_strcmp0 (route.state, "reserved") && g_strcmp0 (route.state, "active") &&
         g_strcmp0 (route.state, "retired")))
        goto out;
    key_dir = g_build_filename (root, route.key, NULL);
    if (!real_directory (key_dir))
        goto out;
    obj_type = g_path_get_basename (native_obj_dir);
    result = g_build_filename (key_dir, "storage", obj_type, repo_id, NULL);
out:
    if (!result)
        seaf_warning ("V2 refused unresolved storage for %.36s.\n", repo_id);
    g_free (route.storage_id);
    g_free (route.key);
    g_free (route.state);
    g_free (root);
    g_free (key_dir);
    g_free (obj_type);
    return result;
}

int
cf_v2_storage_reserve (const char *repo_id)
{
    char *root, *directory, *storage_id;
    SeafDBTrans *trans;
    gboolean db_error = FALSE;
    int ret = -1;
    if (!cf_v2_storage_enabled ())
        return 0;
    if (!is_uuid_valid (repo_id) || !(root = storage_root ()))
        return -1;
    directory = g_build_filename (root, repo_id, NULL);
    storage_id = g_strconcat (AUTO_PREFIX, repo_id, NULL);
    trans = seaf_db_begin_transaction (seaf->db);
    if (!trans)
        goto out;
    /* Never repin an existing library, tombstone, or caller-supplied UUID.
     * UNIQUE(repo_id) serializes concurrent reservations of the same UUID. */
    if (seaf_db_trans_check_for_existence (trans,
            "SELECT 1 FROM RepoStorageId WHERE repo_id=? UNION ALL "
            "SELECT 1 FROM Repo WHERE repo_id=? UNION ALL "
            "SELECT 1 FROM RepoTrash WHERE repo_id=? UNION ALL "
            "SELECT 1 FROM GarbageRepos WHERE repo_id=?", &db_error,
            4, "string", repo_id, "string", repo_id, "string", repo_id, "string", repo_id) || db_error ||
        seaf_db_trans_query (trans,
            "INSERT INTO cf_library_storage (repo_key,repo_id,state) VALUES (?,?,'reserved')",
            2, "string", repo_id, "string", repo_id) < 0 ||
        seaf_db_trans_query (trans,
            "INSERT INTO RepoStorageId (repo_id,storage_id) VALUES (?,?)",
            2, "string", repo_id, "string", storage_id) < 0)
        goto rollback;
    /* Existing untracked directories are not adopted. A failed reservation
     * leaves its directory untouched for operator inspection. */
    if (g_mkdir (directory, 0700) < 0 || !real_directory (directory) ||
        seaf_db_commit (trans) < 0)
        goto rollback;
    ret = 0;
    goto close;
rollback:
    seaf_db_rollback (trans);
close:
    seaf_db_trans_close (trans);
out:
    g_free (storage_id);
    g_free (directory);
    g_free (root);
    return ret;
}

int
cf_v2_storage_activate (const char *repo_id)
{
    if (!cf_v2_storage_enabled ())
        return 0;
    return seaf_db_statement_query (seaf->db,
        "UPDATE cf_library_storage SET state='active' WHERE repo_id=? AND state='reserved'",
        1, "string", repo_id);
}

int
cf_v2_storage_retire (const char *repo_id)
{
    if (!cf_v2_storage_enabled ())
        return 0;
    /* Keep both key and native route for cold GC and never reuse the key. */
    return seaf_db_statement_query (seaf->db,
        "UPDATE cf_library_storage SET state='retired' WHERE repo_id=?",
        1, "string", repo_id);
}

/* Preserve native hardlinks on one volume; stage on the destination volume
 * before publishing a cross-volume copy. No partially copied object is read. */
int
cf_v2_storage_copy (const char *src, const char *dst)
{
    int ret = link (src, dst), input = -1, output = -1;
    char *temporary;
    if (ret == 0 || errno == EEXIST)
        return 0;
    if (errno != EXDEV)
        return -1;
    temporary = g_strconcat (dst, ".XXXXXX", NULL);
    input = g_open (src, O_RDONLY, 0);
    if (input < 0)
        goto out;
    output = g_mkstemp (temporary);
    if (output < 0)
        goto out;
    ret = copy_fd (input, output);
    if (close (output) < 0)
        ret = -1;
    output = -1;
    if (ret == 0)
        ret = g_rename (temporary, dst);
out:
    if (input >= 0)
        close (input);
    if (output >= 0)
        close (output);
    g_unlink (temporary);
    g_free (temporary);
    return ret;
}
