#define _POSIX_C_SOURCE 200809L

/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <fcntl.h>
#include <sys/stat.h>

#include "log.h"
#include "block-backend.h"
#include "cf-s3-client.h"

struct _BHandle {
    char *store_id;
    int version;
    char block_id[41];
    int fd;
    int rw_type;
    char *tmp_file;
};

typedef struct {
    CfS3Client *client;
    char *tmp_dir;
} S3BlockPriv;

typedef struct {
    const char *store_id;
    int version;
    SeafBlockFunc process;
    void *user_data;
    size_t prefix_len;
} ForeachData;

typedef struct {
    GPtrArray *keys;
} RemoveData;

static int
read_block_data (int fd, void *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = read (fd, (char *)buf + total, len - total);
        if (n == 0)
            break;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += n;
    }
    return total;
}

static int
write_block_data (int fd, const void *buf, int len)
{
    int total = 0;
    while (total < len) {
        int n = write (fd, (const char *)buf + total, len - total);
        if (n == 0)
            return -1;
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        total += n;
    }
    return total;
}

static char *
block_key (const char *store_id, const char *block_id)
{
    return g_strdup_printf ("%s/%s", store_id, block_id);
}

static gboolean
valid_block_id (const char *block_id)
{
    int i;

    if (strlen (block_id) != 40)
        return FALSE;
    for (i = 0; i < 40; ++i) {
        if (!g_ascii_isxdigit (block_id[i]))
            return FALSE;
    }
    return TRUE;
}

static int
open_temp (S3BlockPriv *priv, const char *block_id, char **path)
{
    *path = g_strdup_printf ("%s/%s.XXXXXX", priv->tmp_dir, block_id);
    int fd = g_mkstemp (*path);
    if (fd < 0) {
        g_free (*path);
        *path = NULL;
    }
    return fd;
}

static BHandle *
s3_open_block (BlockBackend *bend, const char *store_id, int version,
               const char *block_id, int rw_type)
{
    S3BlockPriv *priv = bend->be_priv;
    BHandle *handle = NULL;
    char *tmp_file = NULL;
    int fd;

    g_return_val_if_fail (block_id != NULL, NULL);
    g_return_val_if_fail (valid_block_id (block_id), NULL);
    g_return_val_if_fail (rw_type == BLOCK_READ ||
                          rw_type == BLOCK_WRITE, NULL);

    fd = open_temp (priv, block_id, &tmp_file);

    if (fd < 0)
        return NULL;

    if (rw_type == BLOCK_READ) {
        char *key = block_key (store_id, block_id);
        FILE *file = fdopen (dup (fd), "wb");
        int ret = file ? cf_s3_client_get_file (priv->client, key, file) : -1;
        if (file)
            fclose (file);
        g_free (key);
        if (ret < 0 || lseek (fd, 0, SEEK_SET) < 0) {
            close (fd);
            g_unlink (tmp_file);
            g_free (tmp_file);
            return NULL;
        }
    }

    handle = g_new0 (BHandle, 1);
    handle->store_id = g_strdup (store_id);
    handle->version = version;
    memcpy (handle->block_id, block_id, 41);
    handle->fd = fd;
    handle->rw_type = rw_type;
    handle->tmp_file = tmp_file;
    return handle;
}

static int
s3_read_block (BlockBackend *bend, BHandle *handle, void *buf, int len)
{
    return read_block_data (handle->fd, buf, len);
}

static int
s3_write_block (BlockBackend *bend, BHandle *handle,
                const void *buf, int len)
{
    return write_block_data (handle->fd, buf, len);
}

static int
s3_close_block (BlockBackend *bend, BHandle *handle)
{
    int ret = close (handle->fd);
    handle->fd = -1;
    return ret;
}

static void
s3_handle_free (BlockBackend *bend, BHandle *handle)
{
    if (handle->fd >= 0)
        close (handle->fd);
    if (handle->tmp_file)
        g_unlink (handle->tmp_file);
    g_free (handle->tmp_file);
    g_free (handle->store_id);
    g_free (handle);
}

static int
s3_commit_block (BlockBackend *bend, BHandle *handle)
{
    S3BlockPriv *priv = bend->be_priv;
    struct stat st;
    FILE *file;
    char *key;
    int ret;

    if (handle->rw_type != BLOCK_WRITE ||
        g_stat (handle->tmp_file, &st) < 0)
        return -1;
    file = g_fopen (handle->tmp_file, "rb");
    if (!file)
        return -1;
    key = block_key (handle->store_id, handle->block_id);
    ret = cf_s3_client_put_file (priv->client, key, file, st.st_size);
    fclose (file);
    g_free (key);
    return ret;
}

static int
s3_block_exists (BlockBackend *bend, const char *store_id, int version,
                 const char *block_id)
{
    S3BlockPriv *priv = bend->be_priv;
    char *key = block_key (store_id, block_id);
    int ret = cf_s3_client_head (priv->client, key, NULL);
    g_free (key);
    return ret;
}

static int
s3_remove_block (BlockBackend *bend, const char *store_id, int version,
                 const char *block_id)
{
    S3BlockPriv *priv = bend->be_priv;
    char *key = block_key (store_id, block_id);
    int ret = cf_s3_client_delete (priv->client, key);
    g_free (key);
    return ret;
}

static BMetadata *
s3_stat_block (BlockBackend *bend, const char *store_id, int version,
               const char *block_id)
{
    S3BlockPriv *priv = bend->be_priv;
    char *key = block_key (store_id, block_id);
    guint64 size = 0;
    int ret = cf_s3_client_head (priv->client, key, &size);
    g_free (key);
    if (ret != 1 || size > G_MAXUINT32)
        return NULL;

    BMetadata *metadata = g_new0 (BMetadata, 1);
    memcpy (metadata->id, block_id, 41);
    metadata->size = (uint32_t)size;
    return metadata;
}

static BMetadata *
s3_stat_handle (BlockBackend *bend, BHandle *handle)
{
    struct stat st;
    if (handle->fd < 0 || fstat (handle->fd, &st) < 0 ||
        st.st_size > G_MAXUINT32)
        return NULL;
    BMetadata *metadata = g_new0 (BMetadata, 1);
    memcpy (metadata->id, handle->block_id, 41);
    metadata->size = (uint32_t)st.st_size;
    return metadata;
}

static gboolean
foreach_listed (const char *key, guint64 size, void *user_data)
{
    ForeachData *data = user_data;
    const char *block_id = key + data->prefix_len;
    if (!valid_block_id (block_id))
        return TRUE;
    return data->process (data->store_id, data->version,
                          block_id, data->user_data);
}

static int
s3_foreach_block (BlockBackend *bend, const char *store_id, int version,
                  SeafBlockFunc process, void *user_data)
{
    S3BlockPriv *priv = bend->be_priv;
    char *prefix = g_strdup_printf ("%s/", store_id);
    ForeachData data = {
        store_id, version, process, user_data, strlen (prefix)
    };
    int ret = cf_s3_client_list (priv->client, prefix,
                                 foreach_listed, &data);
    g_free (prefix);
    return ret;
}

static int
s3_copy_block (BlockBackend *bend, const char *src_store_id,
               int src_version, const char *dst_store_id,
               int dst_version, const char *block_id)
{
    S3BlockPriv *priv = bend->be_priv;
    char *src_key = block_key (src_store_id, block_id);
    char *dst_key = block_key (dst_store_id, block_id);
    char *tmp_path = NULL;
    int fd = -1;
    FILE *write_file = NULL;
    FILE *read_file = NULL;
    struct stat st;
    int ret = -1;

    if (cf_s3_client_head (priv->client, dst_key, NULL) == 1) {
        ret = 0;
        goto out;
    }
    fd = open_temp (priv, block_id, &tmp_path);
    if (fd < 0)
        goto out;
    write_file = fdopen (fd, "wb");
    fd = -1;
    if (!write_file ||
        cf_s3_client_get_file (priv->client, src_key, write_file) < 0)
        goto out;
    fclose (write_file);
    write_file = NULL;
    if (g_stat (tmp_path, &st) < 0)
        goto out;
    read_file = g_fopen (tmp_path, "rb");
    if (!read_file)
        goto out;
    ret = cf_s3_client_put_file (priv->client, dst_key,
                                 read_file, st.st_size);

out:
    if (fd >= 0)
        close (fd);
    if (write_file)
        fclose (write_file);
    if (read_file)
        fclose (read_file);
    if (tmp_path)
        g_unlink (tmp_path);
    g_free (tmp_path);
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
s3_remove_store (BlockBackend *bend, const char *store_id,
                 SeafBlockProgressFunc progress, void *user_data)
{
    S3BlockPriv *priv = bend->be_priv;
    char *prefix = g_strdup_printf ("%s/", store_id);
    RemoveData data = { g_ptr_array_new_with_free_func (g_free) };
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

BlockBackend *
block_backend_s3_new (GKeyFile *config, const char *section,
                      const char *tmp_dir)
{
    BlockBackend *bend = g_new0 (BlockBackend, 1);
    S3BlockPriv *priv = g_new0 (S3BlockPriv, 1);

    priv->client = cf_s3_client_from_config (config, section);
    priv->tmp_dir = g_strdup (tmp_dir);
    if (!priv->client ||
        g_mkdir_with_parents (priv->tmp_dir, 0700) < 0) {
        cf_s3_client_free (priv->client);
        g_free (priv->tmp_dir);
        g_free (priv);
        g_free (bend);
        return NULL;
    }
    bend->be_priv = priv;
    bend->open_block = s3_open_block;
    bend->read_block = s3_read_block;
    bend->write_block = s3_write_block;
    bend->commit_block = s3_commit_block;
    bend->close_block = s3_close_block;
    bend->exists = s3_block_exists;
    bend->remove_block = s3_remove_block;
    bend->stat_block = s3_stat_block;
    bend->stat_block_by_handle = s3_stat_handle;
    bend->block_handle_free = s3_handle_free;
    bend->foreach_block = s3_foreach_block;
    bend->copy = s3_copy_block;
    bend->remove_store = s3_remove_store;
    return bend;
}
