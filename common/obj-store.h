#ifndef OBJ_STORE_H
#define OBJ_STORE_H

#include <glib.h>
#include <sys/types.h>

struct _SeafileSession;
struct SeafObjStore;

struct SeafObjStore *
seaf_obj_store_new (struct _SeafileSession *seaf, const char *obj_type);

int
seaf_obj_store_init (struct SeafObjStore *obj_store);

/* Synchronous I/O interface. */

int
seaf_obj_store_read_obj (struct SeafObjStore *obj_store,
                         const char *repo_id,
                         int version,
                         const char *obj_id,
                         void **data,
                         int *len);

int
seaf_obj_store_write_obj (struct SeafObjStore *obj_store,
                          const char *repo_id,
                          int version,
                          const char *obj_id,
                          void *data,
                          int len,
                          gboolean need_sync);

gboolean
seaf_obj_store_obj_exists (struct SeafObjStore *obj_store,
                           const char *repo_id,
                           int version,
                           const char *obj_id);

/* Returns 1 if present, 0 if absent, and -1 on backend error. */
int
seaf_obj_store_obj_exists_checked (struct SeafObjStore *obj_store,
                                   const char *repo_id,
                                   int version,
                                   const char *obj_id);

int
seaf_obj_store_delete_obj (struct SeafObjStore *obj_store,
                           const char *repo_id,
                           int version,
                           const char *obj_id);

typedef gboolean (*SeafObjFunc) (const char *repo_id,
                                 int version,
                                 const char *obj_id,
                                 void *user_data);

typedef void (*SeafObjProgressFunc) (const char *store_id,
                                     guint64 removed_count,
                                     void *user_data);

int
seaf_obj_store_foreach_obj (struct SeafObjStore *obj_store,
                            const char *repo_id,
                            int version,
                            SeafObjFunc process,
                            void *user_data);

int
seaf_obj_store_copy_obj (struct SeafObjStore *obj_store,
                         const char *src_store_id,
                         int src_version,
                         const char *dst_store_id,
                         int dst_version,
                         const char *obj_id);

int
seaf_obj_store_remove_store (struct SeafObjStore *obj_store,
                             const char *store_id,
                             SeafObjProgressFunc progress_cb,
                             void *user_data);

char *
seaf_obj_store_get_storage_id (struct SeafObjStore *obj_store,
                               const char *repo_id);

gboolean
seaf_obj_store_has_storage_id (struct SeafObjStore *obj_store,
                               const char *storage_id);

int
seaf_obj_store_copy_store (struct SeafObjStore *obj_store,
                           const char *repo_id,
                           int version,
                           const char *src_storage_id,
                           const char *dst_storage_id,
                           SeafObjProgressFunc progress_cb,
                           void *user_data);

#endif
