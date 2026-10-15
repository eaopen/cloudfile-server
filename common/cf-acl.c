/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <string.h>

#include "log.h"
#include "seafile-session.h"
#include "seaf-db.h"
#include "cf-ext.h"
#include "cf-acl.h"
#include "cloudfile-policy.h"

static gboolean cf_acl_on = FALSE;

void
cf_acl_init (void)
{
    if (!seaf || !seaf->cfg_mgr) {
        cf_acl_on = FALSE;
        return;
    }

    /*
     * 2026-10-15, v0.9-only: this provider is no longer the v0.8 rule
     * loader/resolver. That loader queried the removed `subject` column of
     * the unified cf_dir_acl schema, so it failed on every call and denied
     * every library (plain CE ones included) through four call sites. The
     * v0.8 SQL and resolution path are gone; what remains is the
     * managed-library boundary for legacy entries.
     *
     * Registration no longer depends on dir_acl_enabled: once the deployment
     * has the CloudFile managed schema installed (or the guard is explicitly
     * enabled), the legacy RPC and sync entries must refuse managed
     * libraries instead of falling back to native permissions just because
     * the old switch is off. A stock CE install (no cf_* tables, no guard)
     * registers nothing and keeps byte-identical behaviour with zero extra
     * queries on the hot path.
     */
    if (!cf_policy_managed_guard_required (seaf->db, seaf->config)) {
        cf_acl_on = FALSE;
        return;
    }

    cf_acl_on = TRUE;

    /*
     * Register with the extension table rather than having rpc-service.c call
     * cf_acl_* directly.
     *
     * The three upstream files that dispatch permission decisions
     * (rpc-service.c, seaf-server.c, seafile-session.c) are patched once, by
     * the baseline, to call into cf-ext.c. If this capability reached into
     * them itself, every other capability would have to as well, and they
     * would all collide on the same lines -- first between branches, then
     * permanently on dev.
     */
    cf_ext_register ("dir-acl",
                     cf_acl_apply,
                     cf_acl_filter_dirents,
                     cf_acl_find_restricted_path);
}

gboolean
cf_acl_enabled (void)
{
    return cf_acl_on;
}

/* -- managed-library boundary -------------------------------------------- */

/*
 * Whether a legacy entry must refuse this repository.
 *
 * cf_policy_check_legacy_access answers 0 when native (legacy) access is
 * allowed for the repo, and -1 when the repo is managed, the managed schema
 * is missing or drifted, or storage failed -- every failure mode refuses.
 *
 * The four-argument cf_ext seam cannot carry the trusted provider/subject
 * snapshot the v0.9 scoped core needs, so for managed libraries the only
 * safe answer here is "no": returning the native permission would silently
 * bypass the managed rules. Non-managed libraries keep plain CE behaviour,
 * and the v0.9 core adjudicates managed reads on its own native path.
 */
gboolean
cf_acl_legacy_entry_denied (const char *repo_id)
{
    if (!repo_id)
        return FALSE;

    return cf_policy_check_legacy_access (seaf->db, seaf->config, repo_id) != 0;
}

char *
cf_acl_apply (const char *repo_id,
              const char *path,
              const char *user,
              const char *native_perm)
{
    if (!cf_acl_on || !native_perm)
        return g_strdup (native_perm);

    if (!repo_id || !user || *user == '\0')
        return g_strdup (native_perm);

    if (cf_acl_legacy_entry_denied (repo_id)) {
        /* Fail closed: a managed library has no adjudicable legacy answer. */
        return NULL;
    }

    return g_strdup (native_perm);
}

GList *
cf_acl_filter_dirents (const char *repo_id,
                       const char *dir_path,
                       const char *user,
                       GList *dirents)
{
    if (!cf_acl_on || !dirents)
        return dirents;

    if (!repo_id || !user || *user == '\0')
        return dirents;

    if (cf_acl_legacy_entry_denied (repo_id)) {
        /* Fail closed: show nothing rather than list a managed library
         * through an entry that cannot adjudicate its rules. */
        g_list_free_full (dirents, g_object_unref);
        return NULL;
    }

    return dirents;
}

char *
cf_acl_find_restricted_path (const char *repo_id,
                             const char *path,
                             const char *user,
                             const char *native_perm)
{
    if (!cf_acl_on || !native_perm)
        return NULL;

    if (!repo_id || !user || *user == '\0')
        return NULL;

    if (cf_acl_legacy_entry_denied (repo_id)) {
        /* Fail closed: report the root as restricted so the sync or download
         * is refused rather than allowed without an adjudicable answer. */
        return cf_acl_normalize_path (path);
    }

    return NULL;
}
