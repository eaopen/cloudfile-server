/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#ifndef CF_S3_CLIENT_H
#define CF_S3_CLIENT_H

#include <glib.h>
#include <stdio.h>

typedef struct CfS3Client CfS3Client;

typedef gboolean (*CfS3ListFunc) (const char *key,
                                  guint64 size,
                                  void *user_data);

CfS3Client *cf_s3_client_from_config (GKeyFile *config,
                                      const char *section);

CfS3Client *cf_s3_client_new (const char *host,
                              const char *bucket,
                              const char *key_id,
                              const char *secret_key,
                              const char *region,
                              gboolean use_https,
                              gboolean path_style,
                              long connect_timeout,
                              long request_timeout);

void cf_s3_client_free (CfS3Client *client);

int cf_s3_client_get (CfS3Client *client,
                      const char *key,
                      void **data,
                      size_t *len);

int cf_s3_client_get_file (CfS3Client *client,
                           const char *key,
                           FILE *file);

int cf_s3_client_put (CfS3Client *client,
                      const char *key,
                      const void *data,
                      size_t len);

int cf_s3_client_put_file (CfS3Client *client,
                           const char *key,
                           FILE *file,
                           guint64 len);

/* Returns 1 when present, 0 when absent, and -1 on any other error. */
int cf_s3_client_head (CfS3Client *client,
                       const char *key,
                       guint64 *size);

int cf_s3_client_delete (CfS3Client *client,
                         const char *key);

int cf_s3_client_list (CfS3Client *client,
                       const char *prefix,
                       CfS3ListFunc process,
                       void *user_data);

#endif /* CF_S3_CLIENT_H */
