/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include "log.h"
#include "obj-backend.h"
#include "cf-s3-client.h"

typedef struct {
    CfS3Client *client;
} S3ObjPriv;

typedef struct {
    const char *repo_id;
    int version;
    SeafObjFunc process;
    void *user_data;
    size_t prefix_len;
} ForeachData;

typedef struct {
    CfS3Client *client;
    const char *store_id;
    SeafObjProgressFunc progress;
    void *user_data;
    GPtrArray *keys;
} RemoveData;

static char *
object_key (const char *repo_id, const char *obj_id)
{
    return g_strdup_printf ("%s/%s", repo_id, obj_id);
}

static gboolean
valid_object_id (const char *obj_id)
{
    int i;

    if (strlen (obj_id) != 40)
        return FALSE;
    for (i = 0; i < 40; ++i) {
        if (!g_ascii_isxdigit (obj_id[i]))
            return FALSE;
    }
    return TRUE;
}

static int
s3_read (ObjBackend *bend, const char *repo_id, int version,
         const char *obj_id, void **data, int *len)
{
    S3ObjPriv *priv = bend->priv;
    char *key = object_key (repo_id, obj_id);
    size_t result_len = 0;
    int ret = cf_s3_client_get (priv->client, key, data, &result_len);

    g_free (key);
    if (ret < 0 || result_len > G_MAXINT)
        return -1;
    *len = (int)result_len;
    return 0;
}

static int
s3_write (ObjBackend *bend, const char *repo_id, int version,
          const char *obj_id, void *data, int len, gboolean need_sync)
{
    S3ObjPriv *priv = bend->priv;
    char *key = object_key (repo_id, obj_id);
    int ret = len < 0 ? -1 :
        cf_s3_client_put (priv->client, key, data, len);

    g_free (key);
    return ret;
}

static int
s3_exists (ObjBackend *bend, const char *repo_id, int version,
           const char *obj_id)
{
    S3ObjPriv *priv = bend->priv;
    char *key = object_key (repo_id, obj_id);
    int ret = cf_s3_client_head (priv->client, key, NULL);

    g_free (key);
    return ret;
}

static int
s3_delete (ObjBackend *bend, const char *repo_id, int version,
           const char *obj_id)
{
    S3ObjPriv *priv = bend->priv;
    char *key = object_key (repo_id, obj_id);

    int ret = cf_s3_client_delete (priv->client, key);
    if (ret < 0)
        seaf_warning ("Failed to delete S3 object %s.\n", key);
    g_free (key);
    return ret;
}

static gboolean
foreach_listed (const char *key, guint64 size, void *user_data)
{
    ForeachData *data = user_data;
    const char *obj_id = key + data->prefix_len;

    if (!valid_object_id (obj_id))
        return TRUE;
    return data->process (data->repo_id, data->version,
                          obj_id, data->user_data);
}

static int
s3_foreach (ObjBackend *bend, const char *repo_id, int version,
            SeafObjFunc process, void *user_data)
{
    S3ObjPriv *priv = bend->priv;
    char *prefix = g_strdup_printf ("%s/", repo_id);
    ForeachData data = {
        repo_id, version, process, user_data, strlen (prefix)
    };
    int ret = cf_s3_client_list (priv->client, prefix,
                                 foreach_listed, &data);

    g_free (prefix);
    return ret;
}

static int
s3_copy (ObjBackend *bend, const char *src_repo_id, int src_version,
         const char *dst_repo_id, int dst_version, const char *obj_id)
{
    S3ObjPriv *priv = bend->priv;
    char *src_key = object_key (src_repo_id, obj_id);
    char *dst_key = object_key (dst_repo_id, obj_id);
    void *data = NULL;
    size_t len = 0;
    int ret;

    if (cf_s3_client_head (priv->client, dst_key, NULL) == 1) {
        ret = 0;
        goto out;
    }
    ret = cf_s3_client_get (priv->client, src_key, &data, &len);
    if (ret == 0)
        ret = cf_s3_client_put (priv->client, dst_key, data, len);

out:
    g_free (data);
    g_free (src_key);
    g_free (dst_key);
    return ret;
}

static gboolean
collect_key (const char *key, guint64 size, void *user_data)
{
    RemoveData *data = user_data;
    g_ptr_array_add (data->keys, g_strdup (key));
    return TRUE;
}

static int
s3_remove_store (ObjBackend *bend, const char *store_id,
                 SeafObjProgressFunc progress, void *user_data)
{
    S3ObjPriv *priv = bend->priv;
    char *prefix = g_strdup_printf ("%s/", store_id);
    RemoveData data = {
        priv->client, store_id, progress, user_data,
        g_ptr_array_new_with_free_func (g_free)
    };
    guint64 removed = 0;
    guint i;
    int ret = cf_s3_client_list (priv->client, prefix,
                                 collect_key, &data);

    for (i = 0; ret == 0 && i < data.keys->len; ++i) {
        const char *key = g_ptr_array_index (data.keys, i);
        ret = cf_s3_client_delete (priv->client, key);
        if (ret == 0 && progress)
            progress (store_id, ++removed, user_data);
    }
    g_ptr_array_free (data.keys, TRUE);
    g_free (prefix);
    return ret;
}

ObjBackend *
obj_backend_s3_new (GKeyFile *config, const char *section)
{
    ObjBackend *bend = g_new0 (ObjBackend, 1);
    S3ObjPriv *priv = g_new0 (S3ObjPriv, 1);

    priv->client = cf_s3_client_from_config (config, section);
    if (!priv->client) {
        g_free (priv);
        g_free (bend);
        return NULL;
    }

    bend->priv = priv;
    bend->read = s3_read;
    bend->write = s3_write;
    bend->exists = s3_exists;
    bend->delete = s3_delete;
    bend->foreach_obj = s3_foreach;
    bend->copy = s3_copy;
    bend->remove_store = s3_remove_store;
    return bend;
}
