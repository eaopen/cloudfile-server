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

typedef struct SeafDB SeafDB;

/* ------------------------------------------------------------------ config */

/* Read the [cloudfile] section of seafile.conf. Call once at startup, after
 * the config manager exists. Capabilities register from here.
 */
void cf_ext_init (void);

/* Whether a [cloudfile] boolean key is on. Capabilities use this for their
 * own switch, so switch handling stays uniform across them.
 */
gboolean cf_ext_config_bool (const char *key);

/* A [cloudfile] string key, or NULL when unset. Newly allocated; caller
 * frees. Same reason as the boolean above: one place that knows where
 * capability configuration lives.
 */
char *cf_ext_config_string (const char *key);

/* Borrowed configured seafile-db handle; NULL before server initialization. */
SeafDB *cf_ext_database (void);

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
 *
 * @unavailable is optional (may be NULL). A provider must set it TRUE only
 * when it could not read its own authoritative data -- required ACL data unavailable,
 * SQL/transaction failure -- and therefore refuses. It must be
 * left untouched for a deliberate denial. The dispatcher turns that into an
 * explicit RPC error, because reporting an infrastructure failure as "no
 * permission" is indistinguishable to the client from a real denial.
 */
typedef char * (*CfPermFunc) (const char *repo_id,
                              const char *path,
                              const char *user,
                              const char *native_perm,
                              gboolean *unavailable);

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

/* ------------------------------------------- independent qualification */

/*
 * A permission that exists *without* a native library grant.
 *
 * The chain above cannot express one, and must not be bent into doing so: it
 * starts from @native_perm, every provider may only reduce it, and a NULL
 * native permission ends it. A directory share is the opposite case -- it is
 * the normal state of an account that is deliberately not a library member --
 * so a share root would have to start the chain from nothing and *widen*,
 * which no provider in that chain is audited for.
 *
 * Hence this second, explicitly named seam. It is consulted only after
 * seafile_check_permission() returned nothing for @user, and it answers with
 * a permission of its own for that exact path. It may never return "admin" and
 * may never reach a path the capability did not qualify; NULL means "no
 * independent qualification", and callers keep failing closed.
 *
 * Not registered (a stock CE deployment, or every switch off) means the native
 * denial stands, exactly as before this seam existed.
 */
typedef char * (*CfShareGrantFunc) (const char *repo_id,
                                    const char *path,
                                    const char *user,
                                    gboolean *unavailable);

/* Register an independent-qualification capability. */
void cf_ext_register_share_grant (const char *name, CfShareGrantFunc grant);

/*
 * The first qualification any capability will vouch for, or NULL.
 *
 * @unavailable is optional and behaves as in cf_ext_check_permission: TRUE only
 * when the capability that could have qualified the path could not read its own
 * authoritative data, so the caller must answer "unavailable" rather than
 * "denied". A capability that reports unavailable stops the search; a later
 * one could not restore an answer the earlier one failed to compute.
 */
char *cf_ext_share_grant (const char *repo_id,
                          const char *path,
                          const char *user,
                          gboolean *unavailable);

/* ------------------------------------------------------------------ dispatch */

/*
 * Run @native_perm through every registered permission provider, in
 * registration order, each receiving the previous result. Returns a newly
 * allocated string, or NULL for no access. With no provider registered this
 * is a plain g_strdup of @native_perm.
 *
 * @unavailable is optional. It is reset to FALSE on entry and set TRUE only
 * when the provider that refused could not read its authoritative data; the
 * caller must then answer with an explicit unavailable error instead of a
 * permission denial. Callers that cannot distinguish may pass NULL.
 */
char *cf_ext_check_permission (const char *repo_id,
                               const char *path,
                               const char *user,
                               const char *native_perm,
                               gboolean *unavailable);

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
