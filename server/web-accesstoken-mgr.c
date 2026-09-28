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
#include <jansson.h>
#include "cloudfile-acl.h"
#include "cloudfile-policy.h"
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
#ifdef FULL_FEATURE
    gboolean read_only = op && (!strcmp(op, "view") || !strcmp(op, "download") ||
        !strcmp(op, "downloadblks") || !strcmp(op, "download-dir") ||
        !strcmp(op, "download-multi") || !strcmp(op, "download-link") ||
        !strcmp(op, "download-dir-link") || !strcmp(op, "download-multi-link"));
    if (!(read_only && cf_policy_allow_legacy_managed_reads (seaf->config)) &&
        cf_policy_check_legacy_access (seaf->db, seaf->config, repo_id) < 0) {
        g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL, "Legacy library access unavailable");
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
#ifdef FULL_FEATURE
        gboolean read_only = !strcmp(info->op, "view") || !strcmp(info->op, "download") ||
            !strcmp(info->op, "downloadblks") || !strcmp(info->op, "download-dir") ||
            !strcmp(info->op, "download-multi") || !strcmp(info->op, "download-link") ||
            !strcmp(info->op, "download-dir-link") || !strcmp(info->op, "download-multi-link");
        if (!(read_only && cf_policy_allow_legacy_managed_reads (seaf->config)) &&
            cf_policy_check_legacy_access (seaf->db, seaf->config, info->repo_id) < 0) {
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
/* Audit metadata only, never a read grant. Capture immediately after consume
 * and before cleanup/expiry sweep. All fields come from stored native state. */
char *
seaf_web_at_manager_read_transfer_fact (SeafWebAccessTokenManager *mgr, const char *token)
{
    char *result = NULL;
    if (!mgr || !token || strlen (token) != TOKEN_LEN) return NULL;
    pthread_mutex_lock (&mgr->priv->lock);
    AccessInfo *info = g_hash_table_lookup (mgr->priv->access_token_hash, token);
    if (info && info->conditions && info->transferred && info->expire_time > (long)time (NULL)) {
        json_t *conditions = json_loads (info->conditions, JSON_REJECT_DUPLICATES, NULL);
        json_t *context = conditions ? json_object_get (conditions, "context") : NULL;
        const char *user = json_string_value (json_object_get (context, "userId"));
        const char *epoch = json_string_value (json_object_get (context, "epoch"));
        if (user && epoch && info->repo_id && info->path && info->head_id && info->op) {
            json_t *fact = json_pack ("{s:s,s:s,s:s,s:s,s:s,s:s}",
                "user_id", user, "repo_id", info->repo_id, "path", info->path,
                "head_id", info->head_id, "epoch", epoch, "operation", info->op);
            if (fact) {
                char *encoded = json_dumps (fact, JSON_COMPACT);
                result = g_strdup (encoded);
                free (encoded);
                json_decref (fact);
            }
        }
        if (conditions) json_decref (conditions);
    }
    pthread_mutex_unlock (&mgr->priv->lock);
    return result;
}

int
seaf_web_at_manager_end_read_transfer (SeafWebAccessTokenManager *mgr, const char *token)
{
    if (!mgr || !token || strlen (token) != TOKEN_LEN) return -1;
    int result = 0;
    pthread_mutex_lock (&mgr->priv->lock);
    AccessInfo *info = g_hash_table_lookup (mgr->priv->access_token_hash, token);
    if (info && (!info->conditions || !info->transferred)) result = -1;
    else if (info) g_hash_table_remove (mgr->priv->access_token_hash, token);
    pthread_mutex_unlock (&mgr->priv->lock);
    return result;
}

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
    char *transfer_conditions = NULL;
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
    json_t *root = json_loads (copy->conditions, JSON_REJECT_DUPLICATES, NULL);
    if (!json_is_object (root)) {
        if (root) json_decref (root);
        goto out;
    }
    if (json_object_get (root, "user_delegation"))
        json_object_set_new (root, "read_transfer_expires_at", json_integer (copy->expire_time));
    transfer_conditions = json_dumps (root, JSON_COMPACT | JSON_SORT_KEYS);
    json_decref (root);
    if (!transfer_conditions) goto out;
    trans = seaf_db_begin_transaction (mgr->seaf->db);
    if (!trans || seaf_branch_manager_check_read_target (mgr->seaf->branch_mgr, trans,
            copy->repo_id, copy->path, CF_FILE, copy->head_id, copy->obj_id,
            transfer_conditions, copy->username) < 0)
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
    free (transfer_conditions);
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
    json_t *root = conditions ? json_loads (conditions, JSON_REJECT_DUPLICATES, NULL) : NULL;
    gboolean issuance = json_is_object (root) && !json_object_get (root, "read_transfer_expires_at");
    if (root) json_decref (root);
    if (!issuance) goto denied;
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
