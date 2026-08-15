/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* CloudFile storage-class assignment helpers (P2 storage backends). */

#ifndef CF_STORAGE_H
#define CF_STORAGE_H

#include <glib.h>

/* True when [cloudfile] s3_storage_enabled is on (CF_ENABLE_S3_STORAGE). */
gboolean cf_storage_enabled (void);

/* Assign a repo to a storage class by writing/overwriting RepoStorageId.
 * Returns 0 on success, -1 on failure. */
int cf_set_repo_storage_id (const char *repo_id, const char *storage_id);

/* Serialize the configured storage classes into a JSON array of
 * {"storage_id": ..., "storage_name": ..., "is_default": ...}. Caller owns
 * the returned string (g_free). Returns NULL and sets error on failure. */
char *cf_get_storage_classes_json (GError **error);

#endif /* CF_STORAGE_H */
