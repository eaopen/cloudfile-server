/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <string.h>

#include "log.h"
#include "seafile-session.h"
#include "cf-ext.h"

typedef struct CfProvider {
    char *name;
    CfPermFunc perm;
    CfDirentFilterFunc dirent_filter;
    CfRestrictedFunc restricted;
} CfProvider;

static GList *providers = NULL;     /* CfProvider*, registration order */

gboolean
cf_ext_config_bool (const char *key)
{
    if (!seaf || !seaf->cfg_mgr)
        return FALSE;
    return seaf_cfg_manager_get_config_boolean (seaf->cfg_mgr, "cloudfile", key);
}

gboolean
cf_ext_active (void)
{
    return providers != NULL;
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
cf_ext_init (void)
{
    /*
     * Capabilities register themselves here.
     *
     * The baseline registers none, which is the point: with an empty table
     * every hook below is a pass-through and the server behaves exactly like
     * stock CE. A capability branch adds its own cf-*.c and one call here --
     * this file is CloudFile's own, so editing it costs nothing at sync time.
     *
     * e.g. on feature/dir-acl:
     *     cf_acl_init ();
     */
}

char *
cf_ext_check_permission (const char *repo_id,
                         const char *path,
                         const char *user,
                         const char *native_perm)
{
    if (!providers || !native_perm)
        return g_strdup (native_perm);

    char *perm = g_strdup (native_perm);
    GList *ptr;

    for (ptr = providers; ptr; ptr = ptr->next) {
        CfProvider *p = ptr->data;
        if (!p->perm)
            continue;

        char *narrowed = p->perm (repo_id, path, user, perm);
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
