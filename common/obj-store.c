#include "common.h"

#include "log.h"

#include "seafile-session.h"

#include "utils.h"

#include "obj-backend.h"
#include "obj-store.h"
#include "storage-backend-multi.h"

struct SeafObjStore {
    ObjBackend   *bend;
};
typedef struct SeafObjStore SeafObjStore;

extern ObjBackend *
obj_backend_fs_new (const char *seaf_dir, const char *obj_type);
extern ObjBackend *
obj_backend_s3_new (GKeyFile *config, const char *section);

struct SeafObjStore *
seaf_obj_store_new (SeafileSession *seaf, const char *obj_type)
{
    SeafObjStore *store = g_new0 (SeafObjStore, 1);
    const char *section;
    char *name;

    if (!store)
        return NULL;

    if (strcmp (obj_type, "commits") == 0)
        section = "commit_object_backend";
    else if (strcmp (obj_type, "fs") == 0)
        section = "fs_object_backend";
    else {
        seaf_warning ("[Object store] Unknown object type %s.\n", obj_type);
        g_free (store);
        return NULL;
    }

    name = g_key_file_get_string (seaf->config, section, "name", NULL);
    if (!name || strcmp (name, "filesystem") == 0)
        store->bend = obj_backend_fs_new (seaf->seaf_dir, obj_type);
    else if (strcmp (name, "s3") == 0)
        store->bend = obj_backend_s3_new (seaf->config, section);
    else if (strcmp (name, "multiple") == 0)
        store->bend = obj_backend_multi_new (seaf->config, seaf->db,
                                             seaf->seaf_dir, obj_type);
    else
        seaf_warning ("[Object store] Unsupported backend %s in [%s].\n",
                      name, section);
    g_free (name);
    if (!store->bend) {
        seaf_warning ("[Object store] Failed to load backend.\n");
        g_free (store);
        return NULL;
    }

    return store;
}

int
seaf_obj_store_init (SeafObjStore *obj_store)
{
    return 0;
}

int
seaf_obj_store_read_obj (struct SeafObjStore *obj_store,
                         const char *repo_id,
                         int version,
                         const char *obj_id,
                         void **data,
                         int *len)
{
    ObjBackend *bend = obj_store->bend;

    if (!repo_id || !is_uuid_valid(repo_id) ||
        !obj_id || !is_object_id_valid(obj_id))
        return -1;

    return bend->read (bend, repo_id, version, obj_id, data, len);
}

int
seaf_obj_store_write_obj (struct SeafObjStore *obj_store,
                          const char *repo_id,
                          int version,
                          const char *obj_id,
                          void *data,
                          int len,
                          gboolean need_sync)
{
    ObjBackend *bend = obj_store->bend;

    if (!repo_id || !is_uuid_valid(repo_id) ||
        !obj_id || !is_object_id_valid(obj_id))
        return -1;

    return bend->write (bend, repo_id, version, obj_id, data, len, need_sync);
}

gboolean
seaf_obj_store_obj_exists (struct SeafObjStore *obj_store,
                           const char *repo_id,
                           int version,
                           const char *obj_id)
{
    return seaf_obj_store_obj_exists_checked (obj_store, repo_id,
                                              version, obj_id) == 1;
}

int
seaf_obj_store_obj_exists_checked (struct SeafObjStore *obj_store,
                                   const char *repo_id,
                                   int version,
                                   const char *obj_id)
{
    ObjBackend *bend = obj_store->bend;

    if (!repo_id || !is_uuid_valid(repo_id) ||
        !obj_id || !is_object_id_valid(obj_id))
        return -1;

    return bend->exists (bend, repo_id, version, obj_id);
}

int
seaf_obj_store_delete_obj (struct SeafObjStore *obj_store,
                           const char *repo_id,
                           int version,
                           const char *obj_id)
{
    ObjBackend *bend = obj_store->bend;

    if (!repo_id || !is_uuid_valid(repo_id) ||
        !obj_id || !is_object_id_valid(obj_id))
        return -1;

    return bend->delete (bend, repo_id, version, obj_id);
}

int
seaf_obj_store_foreach_obj (struct SeafObjStore *obj_store,
                            const char *repo_id,
                            int version,
                            SeafObjFunc process,
                            void *user_data)
{
    ObjBackend *bend = obj_store->bend;

    return bend->foreach_obj (bend, repo_id, version, process, user_data);
}

int
seaf_obj_store_copy_obj (struct SeafObjStore *obj_store,
                         const char *src_repo_id,
                         int src_version,
                         const char *dst_repo_id,
                         int dst_version,
                         const char *obj_id)
{
    ObjBackend *bend = obj_store->bend;

    if (strcmp (obj_id, EMPTY_SHA1) == 0)
        return 0;

    return bend->copy (bend, src_repo_id, src_version, dst_repo_id, dst_version, obj_id);
}

int
seaf_obj_store_remove_store (struct SeafObjStore *obj_store,
                             const char *store_id,
                             SeafObjProgressFunc progress_cb,
                             void *user_data)
{
    ObjBackend *bend = obj_store->bend;

    return bend->remove_store (bend, store_id, progress_cb, user_data);
}

char *
seaf_obj_store_get_storage_id (struct SeafObjStore *obj_store,
                               const char *repo_id)
{
    ObjBackend *bend = obj_store->bend;

    return bend->get_storage_id ?
        bend->get_storage_id (bend, repo_id) : NULL;
}

gboolean
seaf_obj_store_has_storage_id (struct SeafObjStore *obj_store,
                               const char *storage_id)
{
    ObjBackend *bend = obj_store->bend;

    return bend->has_storage_id ?
        bend->has_storage_id (bend, storage_id) : FALSE;
}

int
seaf_obj_store_copy_store (struct SeafObjStore *obj_store,
                           const char *repo_id,
                           int version,
                           const char *src_storage_id,
                           const char *dst_storage_id,
                           SeafObjProgressFunc progress_cb,
                           void *user_data)
{
    ObjBackend *bend = obj_store->bend;

    return bend->copy_store ?
        bend->copy_store (bend, repo_id, version,
                          src_storage_id, dst_storage_id,
                          progress_cb, user_data) : -1;
}
