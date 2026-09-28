/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include <glib/gstdio.h>

#include "obj-backend.h"
#include "block-backend.h"
#include "cf-s3-client.h"

extern ObjBackend *
obj_backend_s3_new (GKeyFile *config, const char *section);
extern BlockBackend *
block_backend_s3_new (GKeyFile *config, const char *section,
                      const char *tmp_dir);

static const char *commit_repo = "1398a235-74b2-49f7-8ee5-6f6d24d8e13a";
static const char *fs_repo = "2706b4da-08d0-4fd8-9af1-f8871c43c15e";
static const char *block_store = "4f4c5f36-90e8-449d-b6c4-e0acb8c54bc6";
static const char *copy_store = "781fbb87-f63a-4f8f-bf1f-375a330039e8";
static const char *obj_id = "1111111111111111111111111111111111111111";
static const char *missing_id = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static const char *block_id = "2222222222222222222222222222222222222222";
static const char *empty_block_id = "3333333333333333333333333333333333333333";

static int failures;
static int listed;
static int paginated;

#define CHECK(expr, message) do {                                      \
    if (!(expr)) {                                                     \
        fprintf (stderr, "FAIL: %s (%s:%d)\n", message,               \
                 __FILE__, __LINE__);                                  \
        ++failures;                                                    \
    }                                                                  \
} while (0)

static gboolean
count_obj (const char *repo_id, int version,
           const char *listed_id, void *user_data)
{
    if (strcmp (listed_id, obj_id) == 0)
        ++listed;
    return TRUE;
}

static gboolean
count_block (const char *store_id, int version,
             const char *listed_id, void *user_data)
{
    if (strcmp (listed_id, block_id) == 0 ||
        strcmp (listed_id, empty_block_id) == 0)
        ++listed;
    return TRUE;
}

static gboolean
count_paginated (const char *key, guint64 size, void *user_data)
{
    ++paginated;
    return TRUE;
}

static void
test_list_pagination (CfS3Client *client)
{
    char key[96];
    int i;

    for (i = 0; i < 1001; ++i) {
        g_snprintf (key, sizeof(key), "pagination/%040x", i);
        CHECK (cf_s3_client_put (client, key, "", 0) == 0,
               "pagination setup PUT");
    }
    paginated = 0;
    CHECK (cf_s3_client_list (client, "pagination/",
                              count_paginated, NULL) == 0 &&
           paginated == 1001,
           "ListObjectsV2 continuation");
    for (i = 0; i < 1001; ++i) {
        g_snprintf (key, sizeof(key), "pagination/%040x", i);
        CHECK (cf_s3_client_delete (client, key) == 0,
               "pagination cleanup DELETE");
    }
}

static void
test_object_backend (ObjBackend *backend,
                     const char *label,
                     const char *repo_id)
{
    const char payload[] = "cloudfile-s3-object";
    void *result = NULL;
    int len = 0;

    backend->remove_store (backend, repo_id, NULL, NULL);
    CHECK (!backend->exists (backend, repo_id, 1, missing_id),
           "missing object must not exist");
    CHECK (backend->write (backend, repo_id, 1, obj_id,
                           (void *)payload, sizeof(payload) - 1,
                           FALSE) == 0,
           "object PUT");
    CHECK (backend->write (backend, repo_id, 1, obj_id,
                           (void *)payload, sizeof(payload) - 1,
                           FALSE) == 0,
           "repeated object PUT");
    CHECK (backend->exists (backend, repo_id, 1, obj_id),
           "object HEAD");
    CHECK (backend->read (backend, repo_id, 1, obj_id,
                          &result, &len) == 0,
           "object GET");
    CHECK (len == (int)sizeof(payload) - 1 &&
           memcmp (result, payload, len) == 0,
           "object contents");
    g_free (result);

    listed = 0;
    CHECK (backend->foreach_obj (backend, repo_id, 1,
                                 count_obj, NULL) == 0 && listed == 1,
           "object LIST");
    CHECK (backend->copy (backend, repo_id, 1, copy_store, 1,
                          obj_id) == 0,
           "object copy");
    CHECK (backend->exists (backend, copy_store, 1, obj_id),
           "copied object HEAD");
    CHECK (backend->delete (backend, copy_store, 1, obj_id) == 0,
           "object DELETE");
    CHECK (!backend->exists (backend, copy_store, 1, obj_id),
           "object DELETE");
    CHECK (backend->remove_store (backend, repo_id, NULL, NULL) == 0,
           "object remove store");
    CHECK (!backend->exists (backend, repo_id, 1, obj_id),
           "removed object must not exist");
    printf ("PASS C S3 %s backend\n", label);
}

static void
write_block (BlockBackend *backend, const char *store_id,
             const char *id, const void *payload, int len)
{
    BHandle *handle = backend->open_block (
        backend, store_id, 1, id, BLOCK_WRITE);
    CHECK (handle != NULL, "open block for write");
    if (!handle)
        return;
    if (len > 0)
        CHECK (backend->write_block (backend, handle,
                                     payload, len) == len,
               "write block");
    CHECK (backend->close_block (backend, handle) == 0,
           "close written block");
    CHECK (backend->commit_block (backend, handle) == 0,
           "commit block");
    backend->block_handle_free (backend, handle);
}

static void
test_block_backend (BlockBackend *backend)
{
    const char payload[] = "cloudfile-s3-block";
    char buf[64] = { 0 };
    BHandle *handle;
    BMetadata *metadata;
    int n;

    backend->remove_store (backend, block_store, NULL, NULL);
    backend->remove_store (backend, copy_store, NULL, NULL);
    CHECK (!backend->exists (backend, block_store, 1, missing_id),
           "missing block must not exist");
    write_block (backend, block_store, block_id,
                 payload, sizeof(payload) - 1);
    write_block (backend, block_store, empty_block_id, NULL, 0);
    CHECK (backend->exists (backend, block_store, 1, block_id),
           "block HEAD");

    metadata = backend->stat_block (
        backend, block_store, 1, block_id);
    CHECK (metadata && metadata->size == sizeof(payload) - 1,
           "block stat");
    g_free (metadata);
    metadata = backend->stat_block (
        backend, block_store, 1, empty_block_id);
    CHECK (metadata && metadata->size == 0, "empty block stat");
    g_free (metadata);

    handle = backend->open_block (
        backend, block_store, 1, block_id, BLOCK_READ);
    CHECK (handle != NULL, "open block for read");
    if (handle) {
        n = backend->read_block (backend, handle, buf, sizeof(buf));
        CHECK (n == (int)sizeof(payload) - 1 &&
               memcmp (buf, payload, n) == 0,
               "block contents");
        CHECK (backend->read_block (backend, handle,
                                    buf, sizeof(buf)) == 0,
               "block EOF");
        CHECK (backend->close_block (backend, handle) == 0,
               "close read block");
        backend->block_handle_free (backend, handle);
    }

    listed = 0;
    CHECK (backend->foreach_block (backend, block_store, 1,
                                   count_block, NULL) == 0 &&
           listed == 2,
           "block LIST");
    CHECK (backend->copy (backend, block_store, 1, copy_store, 1,
                          block_id) == 0,
           "block copy");
    CHECK (backend->exists (backend, copy_store, 1, block_id),
           "copied block HEAD");
    CHECK (backend->remove_block (backend, copy_store, 1,
                                  block_id) == 0,
           "block DELETE");
    CHECK (backend->remove_store (backend, block_store,
                                  NULL, NULL) == 0,
           "block remove store");
    CHECK (!backend->exists (backend, block_store, 1, block_id),
           "removed block must not exist");
    printf ("PASS C S3 block backend\n");
}

int
main (void)
{
    const char *endpoint = g_getenv ("CF_S3_TEST_ENDPOINT");
    const char *bucket = g_getenv ("CF_S3_TEST_BUCKET");
    const char *key_id = g_getenv ("CF_S3_TEST_KEY_ID");
    const char *secret = g_getenv ("CF_S3_TEST_SECRET_KEY");
    GKeyFile *config;
    ObjBackend *commit_backend;
    ObjBackend *fs_backend;
    BlockBackend *block_backend;
    CfS3Client *unauthorized;
    CfS3Client *missing_bucket;
    CfS3Client *authorized;
    char *tmp_dir;

    if (!bucket || !bucket[0])
        bucket = "cf-s3-c-test";
    if (!key_id || !key_id[0])
        key_id = "minioadmin";
    if (!secret || !secret[0])
        secret = "minioadmin";

    config = g_key_file_new ();
    g_key_file_set_string (config, "s3", "host", endpoint);
    g_key_file_set_string (config, "s3", "bucket", bucket);
    g_key_file_set_string (config, "s3", "key_id", key_id);
    g_key_file_set_string (config, "s3", "key", secret);
    g_key_file_set_boolean (config, "s3", "use_https", FALSE);
    g_key_file_set_boolean (config, "s3", "path_style_request", TRUE);
    g_key_file_set_integer (config, "s3", "request_timeout", 30);

    tmp_dir = g_dir_make_tmp ("cf-s3-blocks-XXXXXX", NULL);
    CHECK (tmp_dir != NULL, "create temporary block directory");
    commit_backend = obj_backend_s3_new (config, "s3");
    fs_backend = obj_backend_s3_new (config, "s3");
    block_backend = block_backend_s3_new (config, "s3", tmp_dir);
    CHECK (commit_backend && fs_backend && block_backend,
           "construct all C S3 backends");

    if (commit_backend)
        test_object_backend (commit_backend, "commit", commit_repo);
    if (fs_backend)
        test_object_backend (fs_backend, "fs", fs_repo);
    if (block_backend)
        test_block_backend (block_backend);

    unauthorized = cf_s3_client_new (
        endpoint, bucket, key_id, "deliberately-wrong-secret",
        "us-east-1", FALSE, TRUE, 2, 5);
    CHECK (unauthorized != NULL, "construct unauthorized S3 client");
    if (unauthorized) {
        CHECK (cf_s3_client_head (unauthorized,
                                  "missing/object", NULL) == -1,
               "403 must remain an error, not become not-found");
        cf_s3_client_free (unauthorized);
    }
    missing_bucket = cf_s3_client_new (
        endpoint, "cf-s3-deliberately-missing-bucket", key_id, secret,
        "us-east-1", FALSE, TRUE, 2, 5);
    CHECK (missing_bucket != NULL, "construct missing-bucket S3 client");
    if (missing_bucket) {
        CHECK (cf_s3_client_head (missing_bucket,
                                  "missing/object", NULL) == -1,
               "missing bucket must remain an error");
        cf_s3_client_free (missing_bucket);
    }
    if (g_getenv ("CF_S3_TEST_PAGINATION")) {
        authorized = cf_s3_client_new (
            endpoint, bucket, key_id, secret, "us-east-1",
            FALSE, TRUE, 2, 30);
        CHECK (authorized != NULL, "construct pagination S3 client");
        if (authorized) {
            test_list_pagination (authorized);
            cf_s3_client_free (authorized);
        }
    }

    g_rmdir (tmp_dir);
    g_free (tmp_dir);
    g_key_file_free (config);
    if (failures) {
        fprintf (stderr, "%d C S3 integration check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
