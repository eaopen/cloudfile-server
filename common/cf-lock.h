/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/* CloudFile's authoritative lease-based file lock backend. */

#ifndef CF_LOCK_H
#define CF_LOCK_H

#include <glib.h>

/* Registers the write-lifecycle provider when [cloudfile] file_lock_enabled
 * is true and lock_backend is either unset or "cloudfile". */
void cf_lock_init (void);

/* JSON RPC adapters. A normal conflict is returned as {"ok":false,...}; a
 * malformed request returns NULL and sets error. The caller owns the string. */
char *cf_lock_status_json (const char *request_json, GError **error);
char *cf_lock_acquire_json (const char *request_json, GError **error);
char *cf_lock_refresh_json (const char *request_json, GError **error);
char *cf_lock_release_json (const char *request_json, GError **error);
char *cf_lock_force_release_json (const char *request_json, GError **error);

gboolean cf_lock_enabled (void);

#endif /* CF_LOCK_H */
