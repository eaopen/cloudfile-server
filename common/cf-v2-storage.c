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

/* Check before constructing any backend, including offline GC sessions. */
int
cf_v2_storage_validate_config (GKeyFile *config)
{
    const char *sections[] = {
        "commit_object_backend", "fs_object_backend", "block_backend"
    };
    guint i;
    if (!g_key_file_has_key (config, "cloudfile_v2", "auto_local_root", NULL))
        return 0;
    if (g_key_file_get_boolean (config, "storage", "enable_storage_classes", NULL)) {
        seaf_warning ("V2 auto_local_root cannot coexist with [storage] enable_storage_classes.\n");
        return -1;
    }
    for (i = 0; i < G_N_ELEMENTS (sections); ++i) {
        char *name = g_key_file_get_string (config, sections[i], "name", NULL);
        gboolean invalid = name && *name && strcmp (name, "filesystem") != 0;
        if (invalid)
            seaf_warning ("V2 auto_local_root requires filesystem backend: [%s] name=%s.\n",
                          sections[i], name);
        g_free (name);
        if (invalid)
            return -1;
    }
    return 0;
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

/* RepoStorageId is the only physical route. The historical CloudFile registry
 * is consulted solely if the native route is absent, to avoid interpreting a
 * lost auto-local mapping as an old CE repo. The allocation marker is not a
 * second routing source or a parallel Repo/Branch lifecycle. */
typedef struct {
    char *storage_id;
    char *legacy_key;
    char *legacy_state;
    gboolean route_row_present;
    gboolean native_repo_present;
} Route;


/* Only verified positive physical routes are cached (not ACLs or failures).
 * Bounds protect other libraries' memory; never hold this mutex over I/O. */
#define ROUTE_CACHE_MAX 1024
#define ROUTE_CACHE_TTL_USEC (10 * G_USEC_PER_SEC)

typedef struct {
    char *repo_id;
    Route route;
    gint64 expires_at;
    GList *link;
} CachedRoute;

static GMutex route_cache_mutex;
static GHashTable *route_cache_index;
static GQueue route_cache_lru;

static void
route_clear (Route *route)
{
    g_free (route->storage_id);
    g_free (route->legacy_key);
    g_free (route->legacy_state);
    memset (route, 0, sizeof(*route));
}

static void
route_copy (Route *dst, const Route *src)
{
    dst->storage_id = g_strdup (src->storage_id);
    dst->legacy_key = g_strdup (src->legacy_key);
    dst->legacy_state = g_strdup (src->legacy_state);
    dst->route_row_present = src->route_row_present;
    dst->native_repo_present = src->native_repo_present;
}

/* Caller holds route_cache_mutex. */
static void
route_cache_remove (CachedRoute *entry)
{
    g_hash_table_remove (route_cache_index, entry->repo_id);
    g_queue_delete_link (&route_cache_lru, entry->link);
    route_clear (&entry->route);
    g_free (entry->repo_id);
    g_free (entry);
}

static gboolean
route_cache_get (const char *repo_id, Route *route)
{
    gboolean hit = FALSE;
    g_mutex_lock (&route_cache_mutex);
    CachedRoute *entry = route_cache_index ?
        g_hash_table_lookup (route_cache_index, repo_id) : NULL;
    if (entry && g_get_monotonic_time () >= entry->expires_at) {
        route_cache_remove (entry);
        entry = NULL;
    }
    if (entry) {
        route_copy (route, &entry->route);
        g_queue_unlink (&route_cache_lru, entry->link);
        g_queue_push_head_link (&route_cache_lru, entry->link);
        hit = TRUE;
    }
    g_mutex_unlock (&route_cache_mutex);
    return hit;
}

static void
route_cache_put (const char *repo_id, const Route *route)
{
    g_mutex_lock (&route_cache_mutex);
    if (!route_cache_index)
        route_cache_index = g_hash_table_new (g_str_hash, g_str_equal);
    CachedRoute *previous = g_hash_table_lookup (route_cache_index, repo_id);
    if (previous)
        route_cache_remove (previous);
    if (g_hash_table_size (route_cache_index) >= ROUTE_CACHE_MAX)
        route_cache_remove (g_queue_peek_tail (&route_cache_lru));
    CachedRoute *entry = g_new0 (CachedRoute, 1);
    entry->repo_id = g_strdup (repo_id);
    route_copy (&entry->route, route);
    entry->expires_at = g_get_monotonic_time () + ROUTE_CACHE_TTL_USEC;
    entry->link = g_list_alloc ();
    entry->link->data = entry;
    g_queue_push_head_link (&route_cache_lru, entry->link);
    g_hash_table_insert (route_cache_index, entry->repo_id, entry);
    g_mutex_unlock (&route_cache_mutex);
}

static void
route_cache_forget (const char *repo_id)
{
    g_mutex_lock (&route_cache_mutex);
    CachedRoute *entry = route_cache_index ?
        g_hash_table_lookup (route_cache_index, repo_id) : NULL;
    if (entry)
        route_cache_remove (entry);
    g_mutex_unlock (&route_cache_mutex);
}

static gboolean
read_route (SeafDBRow *row, void *data)
{
    Route *route = data;
    route->storage_id = g_strdup (seaf_db_row_get_column_text (row, 0));
    route->route_row_present = seaf_db_row_get_column_text (row, 1) != NULL;
    route->legacy_key = g_strdup (seaf_db_row_get_column_text (row, 2));
    route->legacy_state = g_strdup (seaf_db_row_get_column_text (row, 3));
    route->native_repo_present = seaf_db_row_get_column_text (row, 4) != NULL;
    return TRUE;
}

char *
cf_v2_store_dir (const char *native_obj_dir, const char *repo_id)
{
    Route route = {0};
    char *root = NULL, *key_dir = NULL, *obj_type = NULL, *result = NULL;
    const char *key;

    if (!cf_v2_storage_enabled ())
        return g_build_filename (native_obj_dir, repo_id, NULL);
    if (!seaf->db || !repo_id || !is_uuid_valid (repo_id))
        return NULL;

    /* The route itself is the only object location selector. Native lookup
     * and historical markers are needed only when RepoStorageId is absent.
     * A legacy V2 row with a missing route must NEVER become a native write.
     *
     * VirtualRepo counts as an existing native resource: its row is inserted
     * before its initial commit, while the Repo row is written afterwards.
     * Without that fact a folder-share (sub-library) commit would be refused
     * even though it belongs in the native tree, exactly like CE. */
    if (seaf_db_statement_foreach_row (seaf->db,
            "SELECT s.storage_id,s.repo_id,c.repo_key,c.state,"
            "COALESCE(r.repo_id,t.repo_id,g.repo_id,v.repo_id) "
            "FROM (SELECT ? AS repo_id) i "
            "LEFT JOIN RepoStorageId s ON s.repo_id=i.repo_id "
            "LEFT JOIN cf_library_storage c ON c.repo_id=i.repo_id AND s.repo_id IS NULL "
            "LEFT JOIN Repo r ON r.repo_id=i.repo_id AND s.repo_id IS NULL "
            "LEFT JOIN RepoTrash t ON t.repo_id=i.repo_id AND s.repo_id IS NULL "
            "LEFT JOIN GarbageRepos g ON g.repo_id=i.repo_id AND s.repo_id IS NULL "
            "LEFT JOIN VirtualRepo v ON v.repo_id=i.repo_id AND s.repo_id IS NULL",
            read_route, &route, 1, "string", repo_id) != 1)
        goto out;

    if (!route.route_row_present) {
        char *expected = g_strconcat ("native-", repo_id, NULL);
        gboolean recognized = route.native_repo_present &&
            (!route.legacy_key ||
             (g_strcmp0 (route.legacy_key, expected) == 0 &&
              (g_strcmp0 (route.legacy_state, "active") == 0 ||
               g_strcmp0 (route.legacy_state, "retired") == 0)));
        g_free (expected);
        if (recognized)
            result = g_build_filename (native_obj_dir, repo_id, NULL);
        goto out;
    }

    key = route.storage_id && g_str_has_prefix (route.storage_id, AUTO_PREFIX) ?
        route.storage_id + strlen (AUTO_PREFIX) : NULL;
    if (!key_valid (key))
        goto out;

    /* Newly created V2 uses key == repo_id. Old V2 can still use the stored
     * legacy custom suffix. Both resolve with the same path code and no JOIN
     * or lifecycle state is needed to pick the directory. */
    root = storage_root ();
    if (!root)
        goto out;
    key_dir = g_build_filename (root, key, NULL);
    if (!real_directory (key_dir))
        goto out;
    obj_type = g_path_get_basename (native_obj_dir);
    result = g_build_filename (key_dir, "storage", obj_type, repo_id, NULL);
out:
    if (!result)
        seaf_warning ("V2 refused unresolved storage for %.36s.\n", repo_id);
    g_free (route.storage_id);
    g_free (route.legacy_key);
    g_free (route.legacy_state);
    g_free (root);
    g_free (key_dir);
    g_free (obj_type);
    return result;
}

/* New libraries have one native route and one immutable UUID safety marker.
 * Seafile owns Repo/Branch/Owner/Trash/GC: no extra activate state machine.
 * Existing V2 custom-directory rows remain readable without migration. */
int
cf_v2_storage_reserve (const char *repo_id)
{
    char *root, *directory, *storage_id;
    SeafDBTrans *trans = NULL;
    gboolean db_error = FALSE, directory_created = FALSE;
    int ret = -1;

    if (!cf_v2_storage_enabled ())
        return 0;
    if (!seaf->db || !is_uuid_valid (repo_id) || !(root = storage_root ()))
        return -1;

    directory = g_build_filename (root, repo_id, NULL);
    storage_id = g_strconcat (AUTO_PREFIX, repo_id, NULL);
    /* Refuse adoption of any existing directory, even if the DB is empty. */
    if (g_mkdir (directory, 0700) < 0)
        goto out;
    directory_created = TRUE;

    trans = seaf_db_begin_transaction (seaf->db);
    if (!trans)
        goto rollback;
    if (seaf_db_trans_check_for_existence (trans,
            "SELECT 1 FROM RepoStorageId WHERE repo_id=? UNION ALL "
            "SELECT 1 FROM Repo WHERE repo_id=? UNION ALL "
            "SELECT 1 FROM RepoTrash WHERE repo_id=? UNION ALL "
            "SELECT 1 FROM GarbageRepos WHERE repo_id=?", &db_error,
            4, "string", repo_id, "string", repo_id, "string", repo_id, "string", repo_id) ||
        db_error ||
        /* Immutable safety marker, not a second route or lifecycle engine.
         * Prevent an accidentally lost RepoStorageId from looking like CE. */
        seaf_db_trans_query (trans,
            "INSERT INTO cf_library_storage (repo_key,repo_id,state) VALUES (?,?,'active')",
            2, "string", repo_id, "string", repo_id) < 0 ||
        seaf_db_trans_query (trans,
            "INSERT INTO RepoStorageId (repo_id,storage_id) VALUES (?,?)",
            2, "string", repo_id, "string", storage_id) < 0)
        goto rollback;

    if (seaf_db_commit (trans) < 0) {
        /* Commit could have persisted before its acknowledgement was lost.
         * Never delete this directory automatically in an uncertain state. */
        seaf_db_rollback (trans);
        directory_created = FALSE;
        goto close;
    }
    ret = 0;
    goto close;
rollback:
    if (trans)
        seaf_db_rollback (trans);
    if (directory_created && g_rmdir (directory) < 0)
        seaf_warning ("Failed to remove empty V2 allocation directory %.36s.\n", repo_id);
close:
    if (trans)
        seaf_db_trans_close (trans);
out:
    g_free (directory);
    g_free (storage_id);
    g_free (root);
    return ret;
}

/* Mark existing allocation for cold GC; this is not a separate activation
 * stage and does not decide the physical storage directory. */
int
cf_v2_storage_retire (const char *repo_id)
{
    if (!cf_v2_storage_enabled ())
        return 0;
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
    char *temporary, *directory, *name;
    if (ret == 0 || errno == EEXIST)
        return 0;
    if (errno != EXDEV)
        return -1;
    directory = g_path_get_dirname (dst);
    name = g_path_get_basename (dst);
    temporary = g_strdup_printf ("%s/.cf-tmp-%s-XXXXXX", directory, name);
    g_free (directory);
    g_free (name);
    input = g_open (src, O_RDONLY, 0);
    if (input < 0)
        goto out;
    output = g_mkstemp (temporary);
    if (output < 0)
        goto out;
    /* copy_fd() closes its input (and sometimes its output) internally.
     * Keep descriptor ownership here so error cleanup cannot close a reused FD. */
    for (;;) {
        char buffer[8192];
        ssize_t len = readn (input, buffer, sizeof(buffer));
        if (len < 0 || (len > 0 && writen (output, buffer, len) != len)) {
            ret = -1;
            break;
        }
        if (len == 0) {
            ret = fsync (output);
            break;
        }
    }
    if (close (output) < 0)
        ret = -1;
    output = -1;
    if (ret == 0) {
        /* Stage and publish on the destination volume without replacing an
         * object another writer may have published during this copy. Both
         * paths are on the same volume, so link() is atomic and no-clobber. */
        ret = link (temporary, dst);
        if (ret < 0 && errno == EEXIST)
            ret = 0;
    }
out:
    if (input >= 0)
        close (input);
    if (output >= 0)
        close (output);
    g_unlink (temporary);
    g_free (temporary);
    return ret;
}
