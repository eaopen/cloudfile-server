/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*
 * CloudFile server-side extension points.
 *
 * This is the *baseline*: the seams themselves, with no capability behind
 * them. Every hook here is a no-op until something registers a provider, so a
 * CloudFile build with no capabilities enabled behaves exactly like stock CE.
 *
 * Why a dispatch table instead of calling a capability directly:
 *
 *   Capabilities live on long-running feature branches (feature/dir-acl and
 *   the ones after it). If each of them had to patch rpc-service.c,
 *   seaf-server.c and seafile-session.c itself, every upstream sync would cost
 *   once per branch, and the branches would conflict with each other on the
 *   same lines forever.
 *
 *   So the baseline patches those upstream files exactly once, to call in
 *   here. A capability branch then only adds new common/cf-*.c files and one
 *   line to server/Makefile.am -- which is already a declared patch, so its
 *   marginal cost is zero. That is what lets a capability be developed and
 *   improved continuously without re-paying the fork tax.
 *
 * Registration happens during cf_ext_init(); providers are not expected to
 * come and go at runtime.
 */

#ifndef CF_EXT_H
#define CF_EXT_H

#include <glib.h>

/* ------------------------------------------------------------------ config */

/* Read the [cloudfile] section of seafile.conf. Call once at startup, after
 * the config manager exists. Capabilities register from here.
 */
void cf_ext_init (void);

/* Whether a [cloudfile] boolean key is on. Capabilities use this for their
 * own switch, so switch handling stays uniform across them.
 */
gboolean cf_ext_config_bool (const char *key);

/* Whether any capability has registered. Lets callers skip work entirely on a
 * plain CE deployment.
 */
gboolean cf_ext_active (void);

/* ---------------------------------------------------------------- providers */

/*
 * Narrow @native_perm for @user on @path.
 *
 * Must return a newly allocated string, or NULL for no access, and must never
 * return something more privileged than @native_perm -- extensions may only
 * tighten. @native_perm is borrowed.
 */
typedef char * (*CfPermFunc) (const char *repo_id,
                              const char *path,
                              const char *user,
                              const char *native_perm);

/*
 * Drop entries of a directory listing that @user may not see, and rewrite the
 * `permission` property of the rest. Consumes and returns the list.
 */
typedef GList * (*CfDirentFilterFunc) (const char *repo_id,
                                       const char *dir_path,
                                       const char *user,
                                       GList *dirents);

/*
 * First path at or below @path that @user cannot reach at all, or NULL when
 * the whole subtree is reachable. Backs the "may this be synced / packed for
 * download" questions, which have no per-file authorization point once they
 * start.
 */
typedef char * (*CfRestrictedFunc) (const char *repo_id,
                                    const char *path,
                                    const char *user,
                                    const char *native_perm);

/* Register a capability. @name is for logging. Any function may be NULL. */
void cf_ext_register (const char *name,
                      CfPermFunc perm,
                      CfDirentFilterFunc dirent_filter,
                      CfRestrictedFunc restricted);

/* ------------------------------------------------------------------ dispatch */

/*
 * Run @native_perm through every registered permission provider, in
 * registration order, each receiving the previous result. Returns a newly
 * allocated string, or NULL for no access. With no provider registered this
 * is a plain g_strdup of @native_perm.
 */
char *cf_ext_check_permission (const char *repo_id,
                               const char *path,
                               const char *user,
                               const char *native_perm);

/* Run a listing through every registered filter. Consumes @dirents. */
GList *cf_ext_filter_dirents (const char *repo_id,
                              const char *dir_path,
                              const char *user,
                              GList *dirents);

/* First restricted path reported by any provider, or NULL. */
char *cf_ext_find_restricted_path (const char *repo_id,
                                   const char *path,
                                   const char *user,
                                   const char *native_perm);

#endif /* CF_EXT_H */
