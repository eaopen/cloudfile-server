/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <jansson.h>

#include "log.h"
#include "storage-backend-multi.h"

extern ObjBackend *
obj_backend_fs_new (const char *seaf_dir, const char *obj_type);
extern ObjBackend *
obj_backend_s3_new (GKeyFile *config, const char *section);
extern BlockBackend *
block_backend_fs_new (const char *block_dir, const char *tmp_dir);
extern BlockBackend *
block_backend_s3_new (GKeyFile *config, const char *section,
                      const char *tmp_dir);

typedef struct {
    SeafDB *db;
    GHashTable *backends;
    char *default_id;
} MultiPriv;

typedef struct {
    BlockBackend *backend;
    BHandle *handle;
} MultiBlockHandle;

typedef struct {
    ObjBackend *src;
    ObjBackend *dst;
    const char *repo_id;
    int version;
    SeafObjProgressFunc progress;
    void *user_data;
    guint64 copied;
    gboolean failed;
} ObjCopyStoreData;

typedef struct {
    BlockBackend *src;
    BlockBackend *dst;
    const char *store_id;
    int version;
    SeafBlockProgressFunc progress;
    void *user_data;
    guint64 copied;
    gboolean failed;
} BlockCopyStoreData;

static char *
resolve_classes_path (GKeyFile *config, const char *seaf_dir)
{
    char *path = g_key_file_get_string (config, "storage",
                                        "storage_classes_file", NULL);
    char *resolved;

    if (!path || !path[0])
        return path;
    if (g_path_is_absolute (path))
        return path;

    /*
     * A standard installation keeps seafile-data and conf next to each
     * other. Docker writes an absolute path, while this fallback keeps a
     * relative storage_classes_file useful for traditional installations.
     */
    char *top_dir = g_path_get_dirname (seaf_dir);
    resolved = g_build_filename (top_dir, "conf", path, NULL);
    g_free (top_dir);
    g_free (path);
    return resolved;
}

static json_t *
load_storage_classes (GKeyFile *config, const char *seaf_dir)
{
    const char *inline_json = g_getenv ("CF_STORAGE_CLASSES_JSON");
    json_error_t error;
    json_t *classes;

    if (!g_key_file_get_boolean (config, "storage",
                                 "enable_storage_classes", NULL)) {
        seaf_warning ("Multiple storage requires "
                      "[storage] enable_storage_classes = true.\n");
        return NULL;
    }

    if (inline_json && inline_json[0])
        classes = json_loads (inline_json, 0, &error);
    else {
        char *path = resolve_classes_path (config, seaf_dir);
        if (!path || !path[0]) {
            seaf_warning ("Multiple storage requires storage_classes_file "
                          "or CF_STORAGE_CLASSES_JSON.\n");
            g_free (path);
            return NULL;
        }
        classes = json_load_file (path, 0, &error);
        g_free (path);
    }

    if (!classes) {
        seaf_warning ("Failed to parse storage classes at line %d: %s.\n",
                      error.line, error.text);
        return NULL;
    }
    if (!json_is_array (classes) || json_array_size (classes) == 0) {
        seaf_warning ("Storage classes must be a non-empty JSON array.\n");
        json_decref (classes);
        return NULL;
    }
    return classes;
}

static char *
json_scalar_to_string (json_t *value)
{
    if (json_is_string (value))
        return g_strdup (json_string_value (value));
    if (json_is_integer (value))
        return g_strdup_printf ("%" JSON_INTEGER_FORMAT,
                                json_integer_value (value));
    if (json_is_true (value))
        return g_strdup ("true");
    if (json_is_false (value))
        return g_strdup ("false");
    if (json_is_real (value))
        return g_strdup_printf ("%g", json_real_value (value));
    return NULL;
}

static GKeyFile *
key_file_from_spec (json_t *spec)
{
    GKeyFile *config = g_key_file_new ();
    void *iter = json_object_iter (spec);

    while (iter) {
        const char *key = json_object_iter_key (iter);
        json_t *value = json_object_iter_value (iter);
        char *text = json_scalar_to_string (value);
        if (text) {
            g_key_file_set_string (config, "backend", key, text);
            g_free (text);
        }
        iter = json_object_iter_next (spec, iter);
    }
    return config;
}

static ObjBackend *
obj_backend_from_spec (json_t *spec,
                       const char *obj_type)
{
    const char *name;

    if (!json_is_object (spec))
        return NULL;
    name = json_string_value (json_object_get (spec, "backend"));
    if (g_strcmp0 (name, "fs") == 0) {
        const char *dir = json_string_value (json_object_get (spec, "dir"));
        return dir && dir[0] ? obj_backend_fs_new (dir, obj_type) : NULL;
    }
    if (g_strcmp0 (name, "s3") == 0) {
        GKeyFile *config = key_file_from_spec (spec);
        ObjBackend *backend = obj_backend_s3_new (config, "backend");
        g_key_file_free (config);
        return backend;
    }
    return NULL;
}

static BlockBackend *
block_backend_from_spec (json_t *spec,
                         const char *tmp_dir)
{
    const char *name;

    if (!json_is_object (spec))
        return NULL;
    name = json_string_value (json_object_get (spec, "backend"));
    if (g_strcmp0 (name, "fs") == 0) {
        const char *dir = json_string_value (json_object_get (spec, "dir"));
        return dir && dir[0] ?
            block_backend_fs_new (dir, tmp_dir) : NULL;
    }
    if (g_strcmp0 (name, "s3") == 0) {
        GKeyFile *config = key_file_from_spec (spec);
        BlockBackend *backend = block_backend_s3_new (config, "backend",
                                                       tmp_dir);
        g_key_file_free (config);
        return backend;
    }
    return NULL;
}

static MultiPriv *
multi_priv_new (SeafDB *db)
{
    MultiPriv *priv = g_new0 (MultiPriv, 1);

    priv->db = db;
    priv->backends = g_hash_table_new_full (g_str_hash, g_str_equal,
                                            g_free, NULL);
    return priv;
}

static gboolean
read_storage_id (SeafDBRow *row, void *user_data)
{
    char **storage_id = user_data;
    const char *value = seaf_db_row_get_column_text (row, 0);

    if (value)
        *storage_id = g_strdup (value);
    return FALSE;
}

static int
query_storage_id (MultiPriv *priv,
                  const char *repo_id,
                  char **storage_id)
{
    static const char *sql =
        "SELECT storage_id FROM RepoStorageId "
        "WHERE repo_id = COALESCE("
        "(SELECT origin_repo FROM VirtualRepo WHERE repo_id = ?), ?) "
        "LIMIT 1";

    *storage_id = NULL;
    return seaf_db_statement_foreach_row (
        priv->db, sql, read_storage_id, storage_id, 2,
        "string", repo_id, "string", repo_id);
}

static void *
backend_for_repo (MultiPriv *priv, const char *repo_id)
{
    char *storage_id = NULL;
    void *backend;

    if (query_storage_id (priv, repo_id, &storage_id) < 0) {
        seaf_warning ("Failed to resolve storage class for repo %s.\n",
                      repo_id);
        return NULL;
    }
    if (!storage_id)
        storage_id = g_strdup (priv->default_id);

    backend = g_hash_table_lookup (priv->backends, storage_id);
    if (!backend)
        seaf_warning ("Repo %s references unknown storage class %s; "
                      "refusing fallback.\n", repo_id, storage_id);
    g_free (storage_id);
    return backend;
}

static char *
storage_id_for_repo (MultiPriv *priv, const char *repo_id)
{
    char *storage_id = NULL;

    if (query_storage_id (priv, repo_id, &storage_id) < 0)
        return NULL;
    if (!storage_id)
        storage_id = g_strdup (priv->default_id);
    if (!g_hash_table_lookup (priv->backends, storage_id)) {
        g_free (storage_id);
        return NULL;
    }
    return storage_id;
}

static gboolean
multi_has_storage_id (MultiPriv *priv, const char *storage_id)
{
    return storage_id &&
        g_hash_table_lookup (priv->backends, storage_id) != NULL;
}

static gboolean
copy_store_obj (const char *repo_id, int version,
                const char *obj_id, void *user_data)
{
    ObjCopyStoreData *data = user_data;
    void *src_buf = NULL;
    void *dst_buf = NULL;
    int src_len = 0;
    int dst_len = 0;
    int ret;
    int exists;

    ret = data->src->read (data->src, repo_id, version, obj_id,
                           &src_buf, &src_len);
    if (ret == 0) {
        exists = data->dst->exists (data->dst, repo_id, version, obj_id);
        if (exists < 0)
            ret = -1;
        else if (exists == 0)
            ret = data->dst->write (data->dst, repo_id, version, obj_id,
                                    src_buf, src_len, TRUE);
    }
    if (ret == 0)
        ret = data->dst->read (data->dst, repo_id, version, obj_id,
                               &dst_buf, &dst_len);
    if (ret < 0 || src_len != dst_len ||
        (src_len > 0 && memcmp (src_buf, dst_buf, src_len) != 0)) {
        seaf_warning ("Failed to copy and verify object %s:%s.\n",
                      repo_id, obj_id);
        data->failed = TRUE;
    } else {
        ++data->copied;
        if (data->progress)
            data->progress (data->repo_id, data->copied, data->user_data);
    }

    g_free (src_buf);
    g_free (dst_buf);
    return !data->failed;
}

static char *
multi_obj_get_storage_id (ObjBackend *bend, const char *repo_id)
{
    return storage_id_for_repo (bend->priv, repo_id);
}

static gboolean
multi_obj_has_storage_id (ObjBackend *bend, const char *storage_id)
{
    return multi_has_storage_id (bend->priv, storage_id);
}

static int
multi_obj_copy_store (ObjBackend *bend, const char *repo_id, int version,
                      const char *src_storage_id,
                      const char *dst_storage_id,
                      SeafObjProgressFunc progress, void *user_data)
{
    MultiPriv *priv = bend->priv;
    ObjBackend *src = g_hash_table_lookup (priv->backends, src_storage_id);
    ObjBackend *dst = g_hash_table_lookup (priv->backends, dst_storage_id);
    ObjCopyStoreData data = {
        src, dst, repo_id, version, progress, user_data, 0, FALSE
    };
    int ret;

    if (!src || !dst || src == dst)
        return src == dst && src != NULL ? 0 : -1;
    ret = src->foreach_obj (src, repo_id, version,
                            copy_store_obj, &data);
    return ret < 0 || data.failed ? -1 : 0;
}

static int
load_obj_classes (MultiPriv *priv,
                  json_t *classes,
                  const char *obj_type)
{
    size_t index;
    for (index = 0; index < json_array_size (classes); ++index) {
        json_t *item = json_array_get (classes, index);
        const char *storage_id;
        const char *spec_name;
        ObjBackend *backend;

        if (!json_is_object (item))
            return -1;
        storage_id = json_string_value (
            json_object_get (item, "storage_id"));
        if (!storage_id || !storage_id[0] ||
            g_hash_table_lookup_extended (priv->backends, storage_id,
                                          NULL, NULL))
            return -1;
        spec_name = strcmp (obj_type, "commits") == 0 ? "commits" : "fs";
        backend = obj_backend_from_spec (json_object_get (item, spec_name),
                                         obj_type);
        if (!backend) {
            seaf_warning ("Invalid %s backend for storage class %s.\n",
                          obj_type, storage_id);
            return -1;
        }
        g_hash_table_insert (priv->backends, g_strdup (storage_id), backend);
        if (json_is_true (json_object_get (item, "is_default"))) {
            if (priv->default_id)
                return -1;
            priv->default_id = g_strdup (storage_id);
        }
    }
    return priv->default_id ? 0 : -1;
}

static int
load_block_classes (MultiPriv *priv,
                    json_t *classes,
                    const char *tmp_dir)
{
    size_t index;
    for (index = 0; index < json_array_size (classes); ++index) {
        json_t *item = json_array_get (classes, index);
        const char *storage_id;
        BlockBackend *backend;

        if (!json_is_object (item))
            return -1;
        storage_id = json_string_value (
            json_object_get (item, "storage_id"));
        if (!storage_id || !storage_id[0] ||
            g_hash_table_lookup_extended (priv->backends, storage_id,
                                          NULL, NULL))
            return -1;
        backend = block_backend_from_spec (
            json_object_get (item, "blocks"), tmp_dir);
        if (!backend) {
            seaf_warning ("Invalid blocks backend for storage class %s.\n",
                          storage_id);
            return -1;
        }
        g_hash_table_insert (priv->backends, g_strdup (storage_id), backend);
        if (json_is_true (json_object_get (item, "is_default"))) {
            if (priv->default_id)
                return -1;
            priv->default_id = g_strdup (storage_id);
        }
    }
    return priv->default_id ? 0 : -1;
}

static int
multi_obj_read (ObjBackend *bend, const char *repo_id, int version,
                const char *obj_id, void **data, int *len)
{
    ObjBackend *child = backend_for_repo (bend->priv, repo_id);
    return child ? child->read (child, repo_id, version, obj_id,
                                data, len) : -1;
}

static int
multi_obj_write (ObjBackend *bend, const char *repo_id, int version,
                 const char *obj_id, void *data, int len,
                 gboolean need_sync)
{
    ObjBackend *child = backend_for_repo (bend->priv, repo_id);
    return child ? child->write (child, repo_id, version, obj_id,
                                 data, len, need_sync) : -1;
}

static int
multi_obj_exists (ObjBackend *bend, const char *repo_id, int version,
                  const char *obj_id)
{
    ObjBackend *child = backend_for_repo (bend->priv, repo_id);
    return child ? child->exists (child, repo_id, version, obj_id) : -1;
}

static int
multi_obj_delete (ObjBackend *bend, const char *repo_id, int version,
                  const char *obj_id)
{
    ObjBackend *child = backend_for_repo (bend->priv, repo_id);
    return child ? child->delete (child, repo_id, version, obj_id) : -1;
}

static int
multi_obj_foreach (ObjBackend *bend, const char *repo_id, int version,
                   SeafObjFunc process, void *user_data)
{
    ObjBackend *child = backend_for_repo (bend->priv, repo_id);
    return child ? child->foreach_obj (child, repo_id, version,
                                       process, user_data) : -1;
}

static int
multi_obj_copy (ObjBackend *bend,
                const char *src_repo_id, int src_version,
                const char *dst_repo_id, int dst_version,
                const char *obj_id)
{
    ObjBackend *src = backend_for_repo (bend->priv, src_repo_id);
    ObjBackend *dst = backend_for_repo (bend->priv, dst_repo_id);
    void *data = NULL;
    int len = 0;
    int ret;
    int exists;

    if (!src || !dst)
        return -1;
    if (src == dst)
        return src->copy (src, src_repo_id, src_version,
                          dst_repo_id, dst_version, obj_id);
    exists = dst->exists (dst, dst_repo_id, dst_version, obj_id);
    if (exists < 0)
        return -1;
    if (exists == 1)
        return 0;
    ret = src->read (src, src_repo_id, src_version, obj_id, &data, &len);
    if (ret == 0)
        ret = dst->write (dst, dst_repo_id, dst_version, obj_id,
                          data, len, FALSE);
    g_free (data);
    return ret;
}

static int
multi_obj_remove_store (ObjBackend *bend, const char *store_id,
                        SeafObjProgressFunc progress, void *user_data)
{
    MultiPriv *priv = bend->priv;
    GHashTableIter iter;
    gpointer value;
    int ret = 0;

    /*
     * RepoStorageId may already be gone when asynchronous repository cleanup
     * runs. Removing the store prefix from every configured backend is
     * deterministic and avoids either leaking data or guessing a fallback.
     */
    g_hash_table_iter_init (&iter, priv->backends);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        ObjBackend *child = value;
        if (child->remove_store (child, store_id, progress, user_data) < 0)
            ret = -1;
    }
    return ret;
}

ObjBackend *
obj_backend_multi_new (GKeyFile *config,
                       SeafDB *db,
                       const char *seaf_dir,
                       const char *obj_type)
{
    ObjBackend *bend;
    MultiPriv *priv;
    json_t *classes;

    if (!db)
        return NULL;
    classes = load_storage_classes (config, seaf_dir);
    if (!classes)
        return NULL;
    priv = multi_priv_new (db);
    if (load_obj_classes (priv, classes, obj_type) < 0) {
        seaf_warning ("Storage classes require unique storage_id values "
                      "and exactly one default.\n");
        json_decref (classes);
        return NULL;
    }
    json_decref (classes);

    bend = g_new0 (ObjBackend, 1);
    bend->priv = priv;
    bend->read = multi_obj_read;
    bend->write = multi_obj_write;
    bend->exists = multi_obj_exists;
    bend->delete = multi_obj_delete;
    bend->foreach_obj = multi_obj_foreach;
    bend->copy = multi_obj_copy;
    bend->remove_store = multi_obj_remove_store;
    bend->get_storage_id = multi_obj_get_storage_id;
    bend->has_storage_id = multi_obj_has_storage_id;
    bend->copy_store = multi_obj_copy_store;
    return bend;
}

static BHandle *
multi_block_open (BlockBackend *bend, const char *store_id, int version,
                  const char *block_id, int rw_type)
{
    BlockBackend *child = backend_for_repo (bend->be_priv, store_id);
    MultiBlockHandle *handle;
    BHandle *child_handle;

    if (!child)
        return NULL;
    child_handle = child->open_block (child, store_id, version,
                                      block_id, rw_type);
    if (!child_handle)
        return NULL;
    handle = g_new0 (MultiBlockHandle, 1);
    handle->backend = child;
    handle->handle = child_handle;
    return (BHandle *)handle;
}

static int
multi_block_read (BlockBackend *bend, BHandle *handle, void *buf, int len)
{
    MultiBlockHandle *multi = (MultiBlockHandle *)handle;
    return multi->backend->read_block (multi->backend, multi->handle,
                                       buf, len);
}

static int
multi_block_write (BlockBackend *bend, BHandle *handle,
                   const void *buf, int len)
{
    MultiBlockHandle *multi = (MultiBlockHandle *)handle;
    return multi->backend->write_block (multi->backend, multi->handle,
                                        buf, len);
}

static int
multi_block_commit (BlockBackend *bend, BHandle *handle)
{
    MultiBlockHandle *multi = (MultiBlockHandle *)handle;
    return multi->backend->commit_block (multi->backend, multi->handle);
}

static int
multi_block_close (BlockBackend *bend, BHandle *handle)
{
    MultiBlockHandle *multi = (MultiBlockHandle *)handle;
    return multi->backend->close_block (multi->backend, multi->handle);
}

static void
multi_block_handle_free (BlockBackend *bend, BHandle *handle)
{
    MultiBlockHandle *multi = (MultiBlockHandle *)handle;
    multi->backend->block_handle_free (multi->backend, multi->handle);
    g_free (multi);
}

static int
multi_block_exists (BlockBackend *bend, const char *store_id, int version,
                    const char *block_id)
{
    BlockBackend *child = backend_for_repo (bend->be_priv, store_id);
    return child ? child->exists (child, store_id, version, block_id) : -1;
}

static int
multi_block_remove (BlockBackend *bend, const char *store_id, int version,
                    const char *block_id)
{
    BlockBackend *child = backend_for_repo (bend->be_priv, store_id);
    return child ? child->remove_block (child, store_id, version,
                                        block_id) : -1;
}

static BMetadata *
multi_block_stat (BlockBackend *bend, const char *store_id, int version,
                  const char *block_id)
{
    BlockBackend *child = backend_for_repo (bend->be_priv, store_id);
    return child ? child->stat_block (child, store_id, version,
                                      block_id) : NULL;
}

static BMetadata *
multi_block_stat_handle (BlockBackend *bend, BHandle *handle)
{
    MultiBlockHandle *multi = (MultiBlockHandle *)handle;
    return multi->backend->stat_block_by_handle (multi->backend,
                                                  multi->handle);
}

static int
multi_block_foreach (BlockBackend *bend, const char *store_id, int version,
                     SeafBlockFunc process, void *user_data)
{
    BlockBackend *child = backend_for_repo (bend->be_priv, store_id);
    return child ? child->foreach_block (child, store_id, version,
                                         process, user_data) : -1;
}

static int
copy_block_between_backends (BlockBackend *src,
                             const char *src_store_id, int src_version,
                             BlockBackend *dst,
                             const char *dst_store_id, int dst_version,
                             const char *block_id)
{
    BHandle *src_handle = NULL;
    BHandle *dst_handle = NULL;
    char buf[64 * 1024];
    int n;
    int ret = -1;
    int exists;
    gboolean src_closed = FALSE;
    gboolean dst_closed = FALSE;

    exists = dst->exists (dst, dst_store_id, dst_version, block_id);
    if (exists < 0)
        return -1;
    if (exists == 1)
        return 0;
    src_handle = src->open_block (src, src_store_id, src_version,
                                  block_id, BLOCK_READ);
    dst_handle = dst->open_block (dst, dst_store_id, dst_version,
                                  block_id, BLOCK_WRITE);
    if (!src_handle || !dst_handle)
        goto out;
    while ((n = src->read_block (src, src_handle, buf, sizeof(buf))) > 0) {
        if (dst->write_block (dst, dst_handle, buf, n) != n)
            goto out;
    }
    if (n < 0)
        goto out;
    if (src->close_block (src, src_handle) < 0) {
        src->block_handle_free (src, src_handle);
        src_handle = NULL;
        goto out;
    }
    src_closed = TRUE;
    src->block_handle_free (src, src_handle);
    src_handle = NULL;
    if (dst->close_block (dst, dst_handle) < 0)
        goto out;
    dst_closed = TRUE;
    if (dst->commit_block (dst, dst_handle) < 0)
        goto out;
    ret = 0;

out:
    if (src_handle) {
        if (!src_closed)
            src->close_block (src, src_handle);
        src->block_handle_free (src, src_handle);
    }
    if (dst_handle) {
        if (!dst_closed)
            dst->close_block (dst, dst_handle);
        dst->block_handle_free (dst, dst_handle);
    }
    return ret;
}

static int
verify_copied_block (BlockBackend *src, BlockBackend *dst,
                     const char *store_id, int version,
                     const char *block_id)
{
    BHandle *src_handle = NULL;
    BHandle *dst_handle = NULL;
    char src_buf[64 * 1024];
    char dst_buf[64 * 1024];
    int src_n;
    int dst_n;
    int ret = -1;

    src_handle = src->open_block (src, store_id, version,
                                  block_id, BLOCK_READ);
    dst_handle = dst->open_block (dst, store_id, version,
                                  block_id, BLOCK_READ);
    if (!src_handle || !dst_handle)
        goto out;

    do {
        src_n = src->read_block (src, src_handle, src_buf, sizeof(src_buf));
        dst_n = dst->read_block (dst, dst_handle, dst_buf, sizeof(dst_buf));
        if (src_n < 0 || dst_n < 0 || src_n != dst_n)
            goto out;
        if (src_n > 0 && memcmp (src_buf, dst_buf, src_n) != 0)
            goto out;
    } while (src_n > 0);
    ret = 0;

out:
    if (src_handle) {
        src->close_block (src, src_handle);
        src->block_handle_free (src, src_handle);
    }
    if (dst_handle) {
        dst->close_block (dst, dst_handle);
        dst->block_handle_free (dst, dst_handle);
    }
    return ret;
}

static gboolean
copy_store_block (const char *store_id, int version,
                  const char *block_id, void *user_data)
{
    BlockCopyStoreData *data = user_data;

    if (copy_block_between_backends (
            data->src, store_id, version,
            data->dst, store_id, version, block_id) < 0 ||
        verify_copied_block (data->src, data->dst, store_id,
                             version, block_id) < 0) {
        seaf_warning ("Failed to copy and verify block %s:%s.\n",
                      store_id, block_id);
        data->failed = TRUE;
        return FALSE;
    }

    ++data->copied;
    if (data->progress)
        data->progress (data->store_id, data->copied, data->user_data);
    return TRUE;
}

static char *
multi_block_get_storage_id (BlockBackend *bend, const char *store_id)
{
    return storage_id_for_repo (bend->be_priv, store_id);
}

static gboolean
multi_block_has_storage_id (BlockBackend *bend, const char *storage_id)
{
    return multi_has_storage_id (bend->be_priv, storage_id);
}

static int
multi_block_copy_store (BlockBackend *bend,
                        const char *store_id, int version,
                        const char *src_storage_id,
                        const char *dst_storage_id,
                        SeafBlockProgressFunc progress, void *user_data)
{
    MultiPriv *priv = bend->be_priv;
    BlockBackend *src = g_hash_table_lookup (priv->backends, src_storage_id);
    BlockBackend *dst = g_hash_table_lookup (priv->backends, dst_storage_id);
    BlockCopyStoreData data = {
        src, dst, store_id, version, progress, user_data, 0, FALSE
    };
    int ret;

    if (!src || !dst || src == dst)
        return src == dst && src != NULL ? 0 : -1;
    ret = src->foreach_block (src, store_id, version,
                              copy_store_block, &data);
    return ret < 0 || data.failed ? -1 : 0;
}

static int
multi_block_copy (BlockBackend *bend,
                  const char *src_store_id, int src_version,
                  const char *dst_store_id, int dst_version,
                  const char *block_id)
{
    BlockBackend *src = backend_for_repo (bend->be_priv, src_store_id);
    BlockBackend *dst = backend_for_repo (bend->be_priv, dst_store_id);

    if (!src || !dst)
        return -1;
    if (src == dst)
        return src->copy (src, src_store_id, src_version,
                          dst_store_id, dst_version, block_id);
    return copy_block_between_backends (
        src, src_store_id, src_version,
        dst, dst_store_id, dst_version, block_id);
}

static int
multi_block_remove_store (BlockBackend *bend, const char *store_id,
                          SeafBlockProgressFunc progress, void *user_data)
{
    MultiPriv *priv = bend->be_priv;
    GHashTableIter iter;
    gpointer value;
    int ret = 0;

    g_hash_table_iter_init (&iter, priv->backends);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        BlockBackend *child = value;
        if (child->remove_store (child, store_id, progress, user_data) < 0)
            ret = -1;
    }
    return ret;
}

BlockBackend *
block_backend_multi_new (GKeyFile *config,
                         SeafDB *db,
                         const char *seaf_dir,
                         const char *tmp_dir)
{
    BlockBackend *bend;
    MultiPriv *priv;
    json_t *classes;

    if (!db)
        return NULL;
    classes = load_storage_classes (config, seaf_dir);
    if (!classes)
        return NULL;
    priv = multi_priv_new (db);
    if (load_block_classes (priv, classes, tmp_dir) < 0) {
        seaf_warning ("Storage classes require unique storage_id values "
                      "and exactly one default.\n");
        json_decref (classes);
        return NULL;
    }
    json_decref (classes);

    bend = g_new0 (BlockBackend, 1);
    bend->be_priv = priv;
    bend->open_block = multi_block_open;
    bend->read_block = multi_block_read;
    bend->write_block = multi_block_write;
    bend->commit_block = multi_block_commit;
    bend->close_block = multi_block_close;
    bend->exists = multi_block_exists;
    bend->remove_block = multi_block_remove;
    bend->stat_block = multi_block_stat;
    bend->stat_block_by_handle = multi_block_stat_handle;
    bend->block_handle_free = multi_block_handle_free;
    bend->foreach_block = multi_block_foreach;
    bend->copy = multi_block_copy;
    bend->remove_store = multi_block_remove_store;
    bend->get_storage_id = multi_block_get_storage_id;
    bend->has_storage_id = multi_block_has_storage_id;
    bend->copy_store = multi_block_copy_store;
    return bend;
}
