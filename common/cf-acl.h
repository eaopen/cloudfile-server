/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * CloudFile directory-level ACL (v0.8) -- retired to the managed boundary.
 *
 * 2026-10-15, v0.9-only: the v0.8 rule loader and resolver are gone (the
 * unified cf_dir_acl schema has no `subject` column, so that reader failed
 * closed on every call and denied every library). What remains here is the
 * managed-library boundary for legacy entry points: managed libraries are
 * refused because the four-argument cf_ext seam cannot carry the trusted
 * provider/subject snapshot the v0.9 scoped core needs, and non-managed
 * libraries keep plain CE behaviour.
 *
 * The v0.8 policy itself lives on in cf-acl-resolve.h, which depends on
 * nothing but glib so it can be tested against the shared case set in
 * cloudfile-docker/docs/acl-cases.json alongside the Python implementation.
 */

#ifndef CF_ACL_H
#define CF_ACL_H

#include <glib.h>

#include "cf-acl-resolve.h"

/* Decide at startup whether the managed boundary is active. Call once after
 * the config manager exists.
 */
void cf_acl_init (void);

gboolean cf_acl_enabled (void);

/*
 * Whether a legacy entry must refuse @repo_id: managed library, drifted
 * managed schema or storage failure all deny (fail closed). Returns FALSE
 * for plain CE repositories, which keep native behaviour.
 */
gboolean cf_acl_legacy_entry_denied (const char *repo_id);

/*
 * The native permission for @user on @path, or NULL when a legacy entry must
 * refuse the repository.
 *
 * Returns a newly allocated permission string; @native_perm is not consumed.
 * On a stock CE deployment (no CloudFile managed schema, no guard) this is a
 * plain g_strdup of @native_perm.
 */
char *cf_acl_apply (const char *repo_id,
                    const char *path,
                    const char *user,
                    const char *native_perm);

/*
 * First path at or below @path that @user cannot access at all, or NULL when
 * the whole subtree is reachable.
 *
 * Backs "may this repo be synced": the sync protocol trades whole-library
 * commits, so a managed library has to be refused up front.
 */
char *cf_acl_find_restricted_path (const char *repo_id,
                                   const char *path,
                                   const char *user,
                                   const char *native_perm);

/*
 * Apply the boundary to a listing of @dir_path: a managed library yields no
 * listing, anything else returns @dirents unchanged.
 *
 * Consumes @dirents. Returns @dirents unchanged when the boundary is off.
 */
GList *cf_acl_filter_dirents (const char *repo_id,
                              const char *dir_path,
                              const char *user,
                              GList *dirents);

#endif /* CF_ACL_H */
