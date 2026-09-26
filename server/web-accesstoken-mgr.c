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
#include "cloudfile-acl.h"
#include "branch-mgr.h"
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
    /* Non-NULL means CloudFile path-bound: legacy query must not downgrade. */
    char *conditions;
    char *path;
    char *head_id;
    gboolean consuming;
    gboolean transferred;
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
    g_free (info->conditions);
    g_free (info->path);
    g_free (info->head_id);
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

char *
seaf_web_at_manager_get_access_token (SeafWebAccessTokenManager *mgr,
                                      const char *repo_id,
                                      const char *obj_id,
                                      const char *op,
                                      const char *username,
                                      int use_onetime,
                                      GError **error)
{
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
    expire = now + seaf->web_token_expire_time;

    info = g_new0 (AccessInfo, 1);
    info->repo_id = g_strdup (repo_id);
    info->obj_id = g_strdup (obj_id);
    info->op = g_strdup (op);
    info->username = g_strdup (username);
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

SeafileWebAccess *
seaf_web_at_manager_query_access_token (SeafWebAccessTokenManager *mgr,
                                        const char *token)
{
    SeafileWebAccess *webaccess;
    AccessInfo *info;

    pthread_mutex_lock (&mgr->priv->lock);
    info = g_hash_table_lookup (mgr->priv->access_token_hash, token);

    if (info != NULL && info->conditions == NULL) {
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

#ifdef FULL_FEATURE
SeafileWebAccess *
seaf_web_at_manager_consume_read_ticket (SeafWebAccessTokenManager *mgr,
    const char *token, GError **error)
{
    AccessInfo *copy = NULL, *current;
    SeafDBTrans *trans = NULL;
    SeafileWebAccess *result = NULL;
    if (!mgr || !token || strlen (token) != TOKEN_LEN)
        goto denied;
    pthread_mutex_lock (&mgr->priv->lock);
    current = g_hash_table_lookup (mgr->priv->access_token_hash, token);
    if (current && current->conditions && !current->consuming && !current->transferred &&
        current->expire_time > (long)time (NULL)) {
        current->consuming = TRUE;
        copy = g_new0 (AccessInfo, 1);
        copy->repo_id = g_strdup (current->repo_id);
        copy->obj_id = g_strdup (current->obj_id);
        copy->op = g_strdup (current->op);
        copy->username = g_strdup (current->username);
        copy->conditions = g_strdup (current->conditions);
        copy->path = g_strdup (current->path);
        copy->head_id = g_strdup (current->head_id);
        copy->expire_time = current->expire_time;
    }
    pthread_mutex_unlock (&mgr->priv->lock);
    if (!copy)
        goto denied;
    /* Never keep the global ticket mutex across SQL/Redis/FS I/O. The copy
     * owns its strings even if the expiry sweep deletes the claimed entry. */
    trans = seaf_db_begin_transaction (mgr->seaf->db);
    if (!trans || seaf_branch_manager_check_read_target (mgr->seaf->branch_mgr, trans,
            copy->repo_id, copy->path, CF_FILE, copy->head_id, copy->obj_id,
            copy->conditions, copy->username) < 0)
        goto denied;
    pthread_mutex_lock (&mgr->priv->lock);
    current = g_hash_table_lookup (mgr->priv->access_token_hash, token);
    if (current && current->consuming && current->expire_time == copy->expire_time &&
        current->expire_time > (long)time (NULL) &&
        !g_strcmp0 (current->conditions, copy->conditions) &&
        !g_strcmp0 (current->path, copy->path) && !g_strcmp0 (current->head_id, copy->head_id) &&
        !g_strcmp0 (current->obj_id, copy->obj_id) && !g_strcmp0 (current->repo_id, copy->repo_id) &&
        !g_strcmp0 (current->username, copy->username) && !g_strcmp0 (current->op, copy->op) &&
        seaf_db_commit (trans) == 0) {
        result = g_object_new (SEAFILE_TYPE_WEB_ACCESS,
            "repo_id", copy->repo_id, "obj_id", copy->obj_id,
            "op", copy->op, "username", copy->username, NULL);
        current->transferred = TRUE;
        current->consuming = FALSE;
        current->expire_time = (long)time (NULL) + 300;
    }
    pthread_mutex_unlock (&mgr->priv->lock);
    if (result) {
        seaf_db_trans_close (trans);
        free_access_info (copy);
        return result;
    }
denied:
    if (trans) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
    }
    if (copy) {
        pthread_mutex_lock (&mgr->priv->lock);
        current = g_hash_table_lookup (mgr->priv->access_token_hash, token);
        if (current && current->expire_time == copy->expire_time &&
            !g_strcmp0 (current->conditions, copy->conditions))
            current->consuming = FALSE;
        pthread_mutex_unlock (&mgr->priv->lock);
        free_access_info (copy);
    }
    g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL, "CloudFile read ticket unavailable");
    return NULL;
}

int
seaf_web_at_manager_check_read_transfer (SeafWebAccessTokenManager *mgr,
    const char *token)
{
    AccessInfo *copy = NULL;
    SeafDBTrans *trans = NULL;
    int result = -1;
    if (!mgr || !token || strlen (token) != TOKEN_LEN) return -1;
    pthread_mutex_lock (&mgr->priv->lock);
    AccessInfo *current = g_hash_table_lookup (mgr->priv->access_token_hash, token);
    if (current && current->conditions && current->transferred &&
        current->expire_time > (long)time (NULL)) {
        copy = g_new0 (AccessInfo, 1);
        copy->repo_id = g_strdup (current->repo_id);
        copy->obj_id = g_strdup (current->obj_id);
        copy->username = g_strdup (current->username);
        copy->conditions = g_strdup (current->conditions);
        copy->path = g_strdup (current->path);
        copy->head_id = g_strdup (current->head_id);
        copy->expire_time = current->expire_time;
    }
    pthread_mutex_unlock (&mgr->priv->lock);
    if (!copy) return -1;
    trans = seaf_db_begin_transaction (mgr->seaf->db);
    if (!trans || seaf_branch_manager_check_read_target (mgr->seaf->branch_mgr, trans,
            copy->repo_id, copy->path, CF_FILE, copy->head_id, copy->obj_id,
            copy->conditions, copy->username) < 0)
        goto out;
    pthread_mutex_lock (&mgr->priv->lock);
    current = g_hash_table_lookup (mgr->priv->access_token_hash, token);
    if (current && current->transferred && current->expire_time == copy->expire_time &&
        current->expire_time > (long)time (NULL) &&
        !g_strcmp0 (current->conditions, copy->conditions) &&
        !g_strcmp0 (current->path, copy->path) &&
        !g_strcmp0 (current->head_id, copy->head_id) &&
        !g_strcmp0 (current->obj_id, copy->obj_id) &&
        !g_strcmp0 (current->repo_id, copy->repo_id) &&
        !g_strcmp0 (current->username, copy->username) && seaf_db_commit (trans) == 0)
        result = 0;
    pthread_mutex_unlock (&mgr->priv->lock);
out:
    if (trans) {
        if (result < 0) seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
    }
    free_access_info (copy);
    return result;
}

char *
seaf_web_at_manager_issue_read_ticket (SeafWebAccessTokenManager *mgr,
    const char *repo_id, const char *path, const char *head_id, const char *object_id,
    const char *op, const char *username, const char *conditions, GError **error)
{
    char *token = NULL;
    SeafDBTrans *trans = NULL;
    if (!mgr || !op || (strcmp (op, "view") && strcmp (op, "download")))
        goto denied;
    trans = seaf_db_begin_transaction (mgr->seaf->db);
    if (!trans || seaf_branch_manager_check_read_target (mgr->seaf->branch_mgr, trans,
            repo_id, path, CF_FILE, head_id, object_id, conditions, username) < 0)
        goto denied;
    AccessInfo *info = g_new0 (AccessInfo, 1);
    info->repo_id = g_strdup (repo_id);
    info->obj_id = g_strdup (object_id);
    info->op = g_strdup (op);
    info->username = g_strdup (username);
    info->path = g_strdup (path);
    info->head_id = g_strdup (head_id);
    info->conditions = g_strdup (conditions);
    info->use_onetime = TRUE;
    info->expire_time = (long)time (NULL) + 60;
    pthread_mutex_lock (&mgr->priv->lock);
    token = gen_new_token (mgr->priv->access_token_hash);
    g_hash_table_insert (mgr->priv->access_token_hash, g_strdup (token), info);
    /* Do not publish a usable token before its final SQL guard commits. */
    if (seaf_db_commit (trans) < 0) {
        g_hash_table_remove (mgr->priv->access_token_hash, token);
        g_free (token);
        token = NULL;
    }
    pthread_mutex_unlock (&mgr->priv->lock);
    if (token) {
        seaf_db_trans_close (trans);
        return token;
    }
denied:
    if (trans) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
    }
    g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL, "CloudFile read ticket unavailable");
    return NULL;
}
#endif
