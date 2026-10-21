/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"

#include <timer.h>

#include <pthread.h>

#include "seafile-session.h"
#include "web-accesstoken-mgr.h"
#include "seafile-error.h"

#include "utils.h"

#include "log.h"
#ifdef FULL_FEATURE
#include "cf-v2-acl.h"
#endif

#define CLEANING_INTERVAL_MSEC 1000*300	/* 5 minutes */
#define TOKEN_EXPIRE_TIME 3600	        /* 1 hour */
#define TOKEN_LEN 36

struct WebATPriv {
    GHashTable		*access_token_hash; /* token -> access info */
    pthread_mutex_t lock;

    gboolean cluster_mode;
    struct ObjCache *cache;
};
typedef struct WebATPriv WebATPriv;

/* #define DEBUG 1 */

typedef struct {
    char *repo_id;
    char *obj_id;
    char *op;
    char *username;
    long expire_time;
    gboolean use_onetime;
    char *path;
} AccessInfo;

static void
free_access_info (AccessInfo *info)
{
    if (!info)
        return;

    g_free (info->repo_id);
    g_free (info->obj_id);
    g_free (info->op);
    g_free (info->username);
    g_free (info->path);
    g_free (info);
}

SeafWebAccessTokenManager*
seaf_web_at_manager_new (SeafileSession *session)
{
    SeafWebAccessTokenManager *mgr = g_new0 (SeafWebAccessTokenManager, 1);

    mgr->seaf = session;

    mgr->priv = g_new0(WebATPriv, 1);
    mgr->priv->access_token_hash = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                    g_free,
                                                    (GDestroyNotify)free_access_info);
    pthread_mutex_init (&mgr->priv->lock, NULL);

    return mgr;
}

static gboolean
remove_expire_info (gpointer key, gpointer value, gpointer user_data)
{
    AccessInfo *info = (AccessInfo *)value;
    long now = *((long*)user_data);

    if (info && now >= info->expire_time) {
        return TRUE;
    }

    return FALSE;
}

static int
clean_pulse (void *vmanager)
{
    SeafWebAccessTokenManager *manager = vmanager;
    long now = (long)time(NULL);

    pthread_mutex_lock (&manager->priv->lock);

    g_hash_table_foreach_remove (manager->priv->access_token_hash,
                                 remove_expire_info, &now);

    pthread_mutex_unlock (&manager->priv->lock);
    
    return TRUE;
}

int
seaf_web_at_manager_start (SeafWebAccessTokenManager *mgr)
{
    ccnet_timer_new (clean_pulse, mgr, CLEANING_INTERVAL_MSEC);

    return 0;
}

static char *
gen_new_token (GHashTable *token_hash)
{
    char uuid[37];
    char *token;

    while (1) {
        gen_uuid_inplace (uuid);
        token = g_strndup(uuid, TOKEN_LEN);

        /* Make sure the new token doesn't conflict with an existing one. */
        if (g_hash_table_lookup (token_hash, token) != NULL)
            g_free (token);
        else
            return token;
    }
}

static char *
issue_access_token (SeafWebAccessTokenManager *mgr,
                                      const char *repo_id,
                                      const char *obj_id,
                                      const char *op,
                                      const char *username,
                                      int use_onetime,
                                      const char *bound_path,
                                      GError **error)
{
#ifdef FULL_FEATURE
    /* Unbound native tickets cannot prove directory-level authority;
     * the path-bound V2 ticket flow remains available for the same repo. */
    if (!bound_path && cf_v2_acl_unscoped(seaf, repo_id) < 0) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Path-qualified authorization required");
        return NULL;
    }
#endif
    AccessInfo *info;
    long now = (long)time(NULL);
    long expire;
    char *t;
    SeafileWebAccess *webaccess;

    if (strcmp(op, "view") != 0 &&
        strcmp(op, "download") != 0 &&
        strcmp(op, "downloadblks") != 0 &&
        strcmp(op, "download-dir") != 0 &&
        strcmp(op, "download-multi") != 0 &&
        strcmp(op, "download-link") != 0 &&
        strcmp(op, "download-dir-link") != 0 &&
        strcmp(op, "download-multi-link") != 0 &&
        strcmp(op, "upload") != 0 &&
        strcmp(op, "update") != 0 &&
        strcmp(op, "upload-link") != 0 &&
        strcmp(op, "upload-blks-api") != 0 &&
        strcmp(op, "upload-blks-aj") != 0 &&
        strcmp(op, "update-blks-api") != 0 &&
        strcmp(op, "update-blks-aj") != 0) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                     "Invalid operation type.");
        return NULL;
    }

    pthread_mutex_lock (&mgr->priv->lock);

    t = gen_new_token (mgr->priv->access_token_hash);
    /* Keep the Seafile native TTL; scoped file grants never exceed one hour. */
    expire = now + (bound_path ? MIN(3600, seaf->web_token_expire_time)
                               : seaf->web_token_expire_time);

    info = g_new0 (AccessInfo, 1);
    info->repo_id = g_strdup (repo_id);
    info->obj_id = g_strdup (obj_id);
    info->op = g_strdup (op);
    info->username = g_strdup (username);
    info->path = g_strdup (bound_path);
    info->expire_time = expire;
    if (use_onetime) {
        info->use_onetime = TRUE;
    }

    g_hash_table_insert (mgr->priv->access_token_hash, g_strdup(t), info);

#ifdef HAVE_EVHTP
    /* Copy the ZIP task arguments while info is protected. A concurrent
     * expiry sweep or one-time query may free it immediately after unlock. */
    webaccess = NULL;
    if (!seaf->go_fileserver &&
        (strcmp(op, "download-dir") == 0 ||
         strcmp(op, "download-multi") == 0 ||
         strcmp(op, "download-dir-link") == 0 ||
         strcmp(op, "download-multi-link") == 0)) {
        webaccess = g_object_new (SEAFILE_TYPE_WEB_ACCESS,
                                 "repo_id", info->repo_id,
                                 "obj_id", info->obj_id,
                                 "op", info->op,
                                 "username", info->username,
                                 NULL);
    }
#endif

    pthread_mutex_unlock (&mgr->priv->lock);

#ifdef HAVE_EVHTP
    if (!seaf->go_fileserver) {
        if (strcmp(op, "download-dir") == 0 ||
            strcmp(op, "download-multi") == 0 ||
            strcmp(op, "download-dir-link") == 0 ||
            strcmp(op, "download-multi-link") == 0) {

            if (zip_download_mgr_start_zip_task (seaf->zip_download_mgr,
                                                 t, webaccess, error) < 0) {
                pthread_mutex_lock (&mgr->priv->lock);
                g_hash_table_remove (mgr->priv->access_token_hash, t);
                pthread_mutex_unlock (&mgr->priv->lock);

                g_object_unref (webaccess);
                g_free (t);
                return NULL;
            }
            g_object_unref (webaccess);
        }
    }
#endif

    return t;
}

char *
seaf_web_at_manager_get_access_token (SeafWebAccessTokenManager *mgr,
    const char *repo_id, const char *obj_id, const char *op,
    const char *username, int use_onetime, GError **error)
{
    return issue_access_token (mgr, repo_id, obj_id, op, username,
                               use_onetime, NULL, error);
}

#ifdef FULL_FEATURE
/* Same native Seafile token store and TTL. Path marks a ticket whose
 * file/object binding was resolved by Server after Hub authorized the actor. */
char *
seaf_web_at_manager_get_scoped_download_token (SeafWebAccessTokenManager *mgr,
    const char *repo_id, const char *path, const char *obj_id,
    const char *username, int use_onetime, GError **error)
{
    return issue_access_token (mgr, repo_id, obj_id, "download", username,
                               use_onetime, path, error);
}
#endif

SeafileWebAccess *
seaf_web_at_manager_query_access_token (SeafWebAccessTokenManager *mgr,
                                        const char *token)
{
    SeafileWebAccess *webaccess;
    AccessInfo *info;

    pthread_mutex_lock (&mgr->priv->lock);
    info = g_hash_table_lookup (mgr->priv->access_token_hash, token);

    if (info != NULL) {
#ifdef FULL_FEATURE
        if (!info->path && cf_v2_acl_unscoped(seaf, info->repo_id) < 0) {
            pthread_mutex_unlock (&mgr->priv->lock);
            return NULL;
        }
#endif
        long expire_time = info->expire_time;
        long now = (long)time(NULL);        

        if (now - expire_time >= 0) {
            g_hash_table_remove (mgr->priv->access_token_hash, token);
            pthread_mutex_unlock (&mgr->priv->lock);
            return NULL;
        } else {
            webaccess = g_object_new (SEAFILE_TYPE_WEB_ACCESS,
                                      "repo_id", info->repo_id,
                                      "obj_id", info->obj_id,
                                      "op", info->op,
                                      "username", info->username,
                                      "scope-path", info->path,
                                      NULL);

            if (info->use_onetime) {
                g_hash_table_remove (mgr->priv->access_token_hash, token);
            }
            pthread_mutex_unlock (&mgr->priv->lock);
            return webaccess;
        }
    }
    pthread_mutex_unlock (&mgr->priv->lock);
    return NULL;
}
