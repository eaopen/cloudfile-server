/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
#ifndef CF_V2_STORAGE_H
#define CF_V2_STORAGE_H

/* Returned paths are owned by the caller (g_free). NULL means refuse I/O. */
char *cf_v2_store_dir (const char *native_obj_dir, const char *repo_id);
int cf_v2_storage_reserve (const char *repo_id);
int cf_v2_storage_retire (const char *repo_id);
gboolean cf_v2_storage_enabled (void);
int cf_v2_storage_validate_config (GKeyFile *config);
int cf_v2_storage_copy (const char *src, const char *dst);

#endif
