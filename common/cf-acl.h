/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * CloudFile directory-level ACL.
 *
 * This is the authoritative enforcement point. Seahub applies the same rules
 * for the sake of the UI, but WebDAV writes and every RPC caller land here, so
 * a client that talks to seaf-server directly cannot go around it.
 *
 * This header is the Seafile-facing half: config, database and group lookups.
 * The policy itself lives in cf-acl-resolve.h, which depends on nothing but
 * glib so it can be tested against the shared case set in
 * cloudfile-docker/docs/acl-cases.json alongside the Python and Go
 * implementations.
 *
 * Semantics: cloudfile-docker/docs/acl-semantics.md
 */

#ifndef CF_ACL_H
#define CF_ACL_H

#include <glib.h>

#include "cf-acl-resolve.h"

/* Read [cloudfile] dir_acl_enabled from seafile.conf. Call once at startup,
 * after the config manager exists.
 */
void cf_acl_init (void);

gboolean cf_acl_enabled (void);

/*
 * Narrow @native_perm according to the directory ACL for @user on @path.
 *
 * Returns a newly allocated permission string, or NULL for no access.
 * @native_perm is not consumed; pass NULL to get NULL back.
 *
 * When the feature is off this is a plain g_strdup of @native_perm, so the
 * whole call is a no-op on a stock CE deployment.
 */
char *cf_acl_apply (const char *repo_id,
                    const char *path,
                    const char *user,
                    const char *native_perm);

/*
 * First path at or below @path that @user cannot access at all, or NULL when
 * the whole subtree is reachable.
 *
 * Backs "may this repo be synced" and "may this folder be zip-downloaded":
 * both ship an entire subtree in one operation, and neither has a per-file
 * authorization point once it has started, so one unreadable descendant has
 * to block the whole thing up front.
 */
char *cf_acl_find_restricted_path (const char *repo_id,
                                   const char *path,
                                   const char *user,
                                   const char *native_perm);

#endif /* CF_ACL_H */
