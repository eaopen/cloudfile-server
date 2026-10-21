/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <string.h>

#include "log.h"
#include "seafile-session.h"
#include "cf-ext.h"
#include "cf-acl.h"
#include "cf-fileop-test.h"
#include "cf-lock.h"
#include "cf-v2-share.h"

typedef struct CfProvider {
    char *name;
    CfPermFunc perm;
    CfDirentFilterFunc dirent_filter;
    CfRestrictedFunc restricted;
} CfProvider;

/* Independent qualifications, kept apart from the tightening chain on purpose;
 * cf-ext.h explains why they cannot share it. */
typedef struct CfShareProvider {
    char *name;
    CfShareGrantFunc grant;
} CfShareProvider;

static GList *providers = NULL;     /* CfProvider*, registration order */
static GList *share_providers = NULL;  /* CfShareProvider*, registration order */

gboolean
cf_ext_config_bool (const char *key)
{
    if (!seaf || !seaf->cfg_mgr)
        return FALSE;
    return seaf_cfg_manager_get_config_boolean (seaf->cfg_mgr, "cloudfile", key);
}

char *
cf_ext_config_string (const char *key)
{
    if (!seaf || !seaf->cfg_mgr)
        return NULL;
    return seaf_cfg_manager_get_config_string (seaf->cfg_mgr, "cloudfile", key);
}

SeafDB *
cf_ext_database (void)
{
    return seaf ? seaf->db : NULL;
}

gboolean
cf_ext_active (void)
{
    return providers != NULL || share_providers != NULL;
}

void
cf_ext_register (const char *name,
                 CfPermFunc perm,
                 CfDirentFilterFunc dirent_filter,
                 CfRestrictedFunc restricted)
{
    CfProvider *p = g_new0 (CfProvider, 1);
    p->name = g_strdup (name);
    p->perm = perm;
    p->dirent_filter = dirent_filter;
    p->restricted = restricted;

    providers = g_list_append (providers, p);
    seaf_message ("CloudFile: capability '%s' enabled.\n", name);
}

void
cf_ext_register_share_grant (const char *name, CfShareGrantFunc grant)
{
    if (!grant)
        return;

    CfShareProvider *p = g_new0 (CfShareProvider, 1);
    p->name = g_strdup (name);
    p->grant = grant;

    share_providers = g_list_append (share_providers, p);
    seaf_message ("CloudFile: share qualification '%s' enabled.\n", name);
}

void
cf_ext_init (void)
{
    /*
     * Capabilities register themselves here.
     *
     * Current V2 ACL and directory sharing register by installed capability;
     * they are not disabled by an obsolete feature flag. Optional external
     * facilities retain their own real dependency checks. New native hooks
     * must not skip current path authorization when V2 is installed.
     * Adding a capability is one line here plus its own cf-*.c;
     * this file is CloudFile's own, so editing it costs nothing at sync time.
     */
    cf_acl_init ();
    cf_fileop_test_init ();
    cf_lock_init ();
    cf_v2_share_init ();
}

char *
cf_ext_share_grant (const char *repo_id,
                    const char *path,
                    const char *user,
                    gboolean *unavailable)
{
    if (unavailable)
        *unavailable = FALSE;
    if (!share_providers || !repo_id || !path || !user || !*user)
        return NULL;

    GList *ptr;
    for (ptr = share_providers; ptr; ptr = ptr->next) {
        CfShareProvider *p = ptr->data;
        char *grant = p->grant (repo_id, path, user, unavailable);
        if (grant)
            return grant;
        /* Its own authoritative data was unreadable: no later capability can
         * answer for it, and "denied" would misreport an outage. */
        if (unavailable && *unavailable)
            return NULL;
    }

    return NULL;
}

char *
cf_ext_check_permission (const char *repo_id,
                         const char *path,
                         const char *user,
                         const char *native_perm,
                         gboolean *unavailable)
{
    if (unavailable)
        *unavailable = FALSE;
    if (!providers || !native_perm)
        return g_strdup (native_perm);

    char *perm = g_strdup (native_perm);
    GList *ptr;

    for (ptr = providers; ptr; ptr = ptr->next) {
        CfProvider *p = ptr->data;
        if (!p->perm)
            continue;

        char *narrowed = p->perm (repo_id, path, user, perm, unavailable);
        g_free (perm);
        perm = narrowed;

        /* Denied outright -- no later provider can widen it back. */
        if (!perm)
            return NULL;
    }

    return perm;
}

GList *
cf_ext_filter_dirents (const char *repo_id,
                       const char *dir_path,
                       const char *user,
                       GList *dirents)
{
    GList *ptr;

    for (ptr = providers; ptr; ptr = ptr->next) {
        CfProvider *p = ptr->data;
        if (p->dirent_filter)
            dirents = p->dirent_filter (repo_id, dir_path, user, dirents);
    }

    return dirents;
}

char *
cf_ext_find_restricted_path (const char *repo_id,
                             const char *path,
                             const char *user,
                             const char *native_perm)
{
    GList *ptr;

    for (ptr = providers; ptr; ptr = ptr->next) {
        CfProvider *p = ptr->data;
        if (!p->restricted)
            continue;

        char *restricted = p->restricted (repo_id, path, user, native_perm);
        if (restricted)
            return restricted;      /* first one that says no wins */
    }

    return NULL;
}
