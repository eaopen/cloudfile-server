#include "common.h"

#include "log.h"

#ifndef SEAFILE_SERVER
#include "db.h"
#else
#include "seaf-db.h"
#endif

#include "seafile-session.h"

#ifdef FULL_FEATURE
#include "notif-mgr.h"
#include "cloudfile-policy.h"
#include "cloudfile-acl.h"
#include "repo-mgr.h"
#include "fs-mgr.h"
#include "utils.h"
#endif

#include "branch-mgr.h"
#if defined(SEAFILE_SERVER) && defined(FULL_FEATURE)
#include <jansson.h>
#include <hiredis.h>
#endif

#define BRANCH_DB "branch.db"

SeafBranch *
seaf_branch_new (const char *name, const char *repo_id, const char *commit_id)
{
    SeafBranch *branch;

    branch = g_new0 (SeafBranch, 1);

    branch->name = g_strdup (name);
    memcpy (branch->repo_id, repo_id, 36);
    branch->repo_id[36] = '\0';
    memcpy (branch->commit_id, commit_id, 40);
    branch->commit_id[40] = '\0';

    branch->ref = 1;

    return branch;
}

void
seaf_branch_free (SeafBranch *branch)
{
    if (branch == NULL) return;
    g_free (branch->name);
    g_free (branch);
}

void
seaf_branch_list_free (GList *blist)
{
    GList *ptr;

    for (ptr = blist; ptr; ptr = ptr->next) {
        seaf_branch_unref (ptr->data);
    }
    g_list_free (blist);
}


void
seaf_branch_set_commit (SeafBranch *branch, const char *commit_id)
{
    memcpy (branch->commit_id, commit_id, 40);
    branch->commit_id[40] = '\0';
}

void
seaf_branch_ref (SeafBranch *branch)
{
    branch->ref++;
}

void
seaf_branch_unref (SeafBranch *branch)
{
    if (!branch)
        return;

    if (--branch->ref <= 0)
        seaf_branch_free (branch);
}

struct _SeafBranchManagerPriv {
    sqlite3 *db;
#ifndef SEAFILE_SERVER
    pthread_mutex_t db_lock;
#endif
};

static int open_db (SeafBranchManager *mgr);

SeafBranchManager *
seaf_branch_manager_new (struct _SeafileSession *seaf)
{
    SeafBranchManager *mgr;

    mgr = g_new0 (SeafBranchManager, 1);
    mgr->priv = g_new0 (SeafBranchManagerPriv, 1);
    mgr->seaf = seaf;

#ifndef SEAFILE_SERVER
    pthread_mutex_init (&mgr->priv->db_lock, NULL);
#endif

    return mgr;
}

int
seaf_branch_manager_init (SeafBranchManager *mgr)
{
    return open_db (mgr);
}

static int
open_db (SeafBranchManager *mgr)
{
    if (!mgr->seaf->create_tables && seaf_db_type (mgr->seaf->db) != SEAF_DB_TYPE_PGSQL)
        return 0;
#ifndef SEAFILE_SERVER

    char *db_path;
    const char *sql;

    db_path = g_build_filename (mgr->seaf->seaf_dir, BRANCH_DB, NULL);
    if (sqlite_open_db (db_path, &mgr->priv->db) < 0) {
        g_critical ("[Branch mgr] Failed to open branch db\n");
        g_free (db_path);
        return -1;
    }
    g_free (db_path);

    sql = "CREATE TABLE IF NOT EXISTS Branch ("
          "name TEXT, repo_id TEXT, commit_id TEXT);";
    if (sqlite_query_exec (mgr->priv->db, sql) < 0)
        return -1;

    sql = "CREATE INDEX IF NOT EXISTS branch_index ON Branch(repo_id, name);";
    if (sqlite_query_exec (mgr->priv->db, sql) < 0)
        return -1;

#elif defined FULL_FEATURE

    char *sql;
    switch (seaf_db_type (mgr->seaf->db)) {
    case SEAF_DB_TYPE_MYSQL:
        sql = "CREATE TABLE IF NOT EXISTS Branch ("
            "id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT, "
            "name VARCHAR(10), repo_id CHAR(41), commit_id CHAR(41),"
            "UNIQUE INDEX(repo_id, name)) ENGINE = INNODB";
        if (seaf_db_query (mgr->seaf->db, sql) < 0)
            return -1;
        break;
    case SEAF_DB_TYPE_PGSQL:
        sql = "CREATE TABLE IF NOT EXISTS Branch ("
            "name VARCHAR(10), repo_id CHAR(40), commit_id CHAR(40),"
            "PRIMARY KEY (repo_id, name))";
        if (seaf_db_query (mgr->seaf->db, sql) < 0)
            return -1;
        break;
    case SEAF_DB_TYPE_SQLITE:
        sql = "CREATE TABLE IF NOT EXISTS Branch ("
            "name VARCHAR(10), repo_id CHAR(41), commit_id CHAR(41),"
            "PRIMARY KEY (repo_id, name))";
        if (seaf_db_query (mgr->seaf->db, sql) < 0)
            return -1;
        break;
    }

#endif

    return 0;
}

int
seaf_branch_manager_add_branch (SeafBranchManager *mgr, SeafBranch *branch)
{
#ifndef SEAFILE_SERVER
    char sql[256];

    pthread_mutex_lock (&mgr->priv->db_lock);

    sqlite3_snprintf (sizeof(sql), sql,
                      "SELECT 1 FROM Branch WHERE name=%Q and repo_id=%Q",
                      branch->name, branch->repo_id);
    if (sqlite_check_for_existence (mgr->priv->db, sql))
        sqlite3_snprintf (sizeof(sql), sql,
                          "UPDATE Branch SET commit_id=%Q WHERE "
                          "name=%Q and repo_id=%Q",
                          branch->commit_id, branch->name, branch->repo_id);
    else
        sqlite3_snprintf (sizeof(sql), sql,
                          "INSERT INTO Branch (name, repo_id, commit_id) VALUES (%Q, %Q, %Q)",
                          branch->name, branch->repo_id, branch->commit_id);

    sqlite_query_exec (mgr->priv->db, sql);

    pthread_mutex_unlock (&mgr->priv->db_lock);

    return 0;
#else
#ifdef FULL_FEATURE
    if (cf_policy_managed_guard_required (mgr->seaf->db, mgr->seaf->config)) {
        SeafDBTrans *trans = seaf_db_begin_transaction (mgr->seaf->db);
        if (!trans) return -1;
        int rc = cf_policy_check_legacy_library (trans, branch->repo_id);
        if (rc == 0)
            rc = seaf_db_trans_query (trans,
                "REPLACE INTO Branch(name,repo_id,commit_id) VALUES(?,?,?)",
                3, "string", branch->name, "string", branch->repo_id, "string", branch->commit_id);
        if (rc == 0) rc = seaf_db_commit (trans);
        if (rc < 0) seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return rc;
    }
#endif
    char *sql;
    SeafDB *db = mgr->seaf->db;

    if (seaf_db_type(db) == SEAF_DB_TYPE_PGSQL) {
        gboolean exists, err;
        int rc;

        sql = "SELECT repo_id FROM Branch WHERE name=? AND repo_id=?";
        exists = seaf_db_statement_exists(db, sql, &err,
                                          2, "string", branch->name,
                                          "string", branch->repo_id);
        if (err)
            return -1;

        if (exists)
            rc = seaf_db_statement_query (db,
                                          "UPDATE Branch SET commit_id=? "
                                          "WHERE name=? AND repo_id=?",
                                          3, "string", branch->commit_id,
                                          "string", branch->name,
                                          "string", branch->repo_id);
        else
            rc = seaf_db_statement_query (db,
                                          "INSERT INTO Branch (name, repo_id, commit_id) VALUES (?, ?, ?)",
                                          3, "string", branch->name,
                                          "string", branch->repo_id,
                                          "string", branch->commit_id);
        if (rc < 0)
            return -1;
    } else {
        int rc = seaf_db_statement_query (db,
                                 "REPLACE INTO Branch (name, repo_id, commit_id) VALUES (?, ?, ?)",
                                 3, "string", branch->name,
                                 "string", branch->repo_id,
                                 "string", branch->commit_id);
        if (rc < 0)
            return -1;
    }
    return 0;
#endif
}

int
seaf_branch_manager_del_branch (SeafBranchManager *mgr,
                                const char *repo_id,
                                const char *name)
{
#ifndef SEAFILE_SERVER
    char *sql;

    pthread_mutex_lock (&mgr->priv->db_lock);

    sql = sqlite3_mprintf ("DELETE FROM Branch WHERE name = %Q AND "
                           "repo_id = '%s'", name, repo_id);
    if (sqlite_query_exec (mgr->priv->db, sql) < 0)
        seaf_warning ("Delete branch %s failed\n", name);
    sqlite3_free (sql);

    pthread_mutex_unlock (&mgr->priv->db_lock);

    return 0;
#else
#ifdef FULL_FEATURE
    if (cf_policy_managed_guard_required (mgr->seaf->db, mgr->seaf->config)) {
        SeafDBTrans *trans = seaf_db_begin_transaction (mgr->seaf->db);
        if (!trans) return -1;
        int rc = cf_policy_check_legacy_library (trans, repo_id);
        if (rc == 0)
            rc = seaf_db_trans_query (trans, "DELETE FROM Branch WHERE name=? AND repo_id=?",
                2, "string", name, "string", repo_id);
        if (rc == 0) rc = seaf_db_commit (trans);
        if (rc < 0) seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return rc;
    }
#endif
    int rc = seaf_db_statement_query (mgr->seaf->db,
                                      "DELETE FROM Branch WHERE name=? AND repo_id=?",
                                      2, "string", name, "string", repo_id);
    if (rc < 0)
        return -1;
    return 0;
#endif
}

int
seaf_branch_manager_update_branch (SeafBranchManager *mgr, SeafBranch *branch)
{
#ifndef SEAFILE_SERVER
    sqlite3 *db;
    char *sql;

    pthread_mutex_lock (&mgr->priv->db_lock);

    db = mgr->priv->db;
    sql = sqlite3_mprintf ("UPDATE Branch SET commit_id = %Q "
                           "WHERE name = %Q AND repo_id = %Q",
                           branch->commit_id, branch->name, branch->repo_id);
    sqlite_query_exec (db, sql);
    sqlite3_free (sql);

    pthread_mutex_unlock (&mgr->priv->db_lock);

    return 0;
#else
#ifdef FULL_FEATURE
    if (cf_policy_managed_guard_required (mgr->seaf->db, mgr->seaf->config)) {
        SeafDBTrans *trans = seaf_db_begin_transaction (mgr->seaf->db);
        if (!trans) return -1;
        int rc = cf_policy_check_legacy_library (trans, branch->repo_id);
        if (rc == 0)
            rc = seaf_db_trans_query (trans,
                "UPDATE Branch SET commit_id=? WHERE name=? AND repo_id=?",
                3, "string", branch->commit_id, "string", branch->name, "string", branch->repo_id);
        if (rc == 0) rc = seaf_db_commit (trans);
        if (rc < 0) seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return rc;
    }
#endif
    int rc = seaf_db_statement_query (mgr->seaf->db,
                                      "UPDATE Branch SET commit_id = ? "
                                      "WHERE name = ? AND repo_id = ?",
                                      3, "string", branch->commit_id,
                                      "string", branch->name,
                                      "string", branch->repo_id);
    if (rc < 0)
        return -1;
    return 0;
#endif
}

#if defined( SEAFILE_SERVER ) && defined( FULL_FEATURE )

#include "mq-mgr.h"

static gboolean
get_commit_id (SeafDBRow *row, void *data)
{
    char *out_commit_id = data;
    const char *commit_id;

    commit_id = seaf_db_row_get_column_text (row, 0);
    memcpy (out_commit_id, commit_id, 41);
    out_commit_id[40] = '\0';

    return FALSE;
}

static void
publish_repo_update_event (const char *repo_id, const char *commit_id)
{
    json_t *msg = json_object ();
    char *msg_str = NULL;

    json_object_set_new (msg, "msg_type", json_string("repo-update"));
    json_object_set_new (msg, "repo_id", json_string(repo_id));
    json_object_set_new (msg, "commit_id", json_string(commit_id));

    msg_str = json_dumps (msg, JSON_PRESERVE_ORDER);

    seaf_mq_manager_publish_event (seaf->mq_mgr, SEAFILE_SERVER_CHANNEL_EVENT, msg_str);
    g_free (msg_str);
    json_decref (msg);
}

static void
notify_repo_update (const char *repo_id, const char *commit_id)
{
    json_t *event = NULL;
    json_t *content = NULL;
    char *msg = NULL;

    event = json_object ();
    content = json_object ();

    json_object_set_new (event, "type", json_string("repo-update"));

    json_object_set_new (content, "repo_id", json_string(repo_id));
    json_object_set_new (content, "commit_id", json_string(commit_id));

    json_object_set_new (event, "content", content);

    msg = json_dumps (event, JSON_COMPACT);

    if (seaf->notif_mgr)
        seaf_notif_manager_send_event (seaf->notif_mgr, msg);

    json_decref (event);
    g_free (msg);
}

static void
on_branch_updated (SeafBranchManager *mgr, SeafBranch *branch)
{
    if (seaf->is_repair)
        return;
    seaf_repo_manager_update_repo_info (seaf->repo_mgr, branch->repo_id, branch->commit_id);

    notify_repo_update(branch->repo_id, branch->commit_id);

    if (seaf_repo_manager_is_virtual_repo (seaf->repo_mgr, branch->repo_id))
        return;

    publish_repo_update_event (branch->repo_id, branch->commit_id);
}

static gboolean
get_gc_id (SeafDBRow *row, void *data)
{
    char **out_gc_id = data;

    *out_gc_id = g_strdup(seaf_db_row_get_column_text (row, 0));

    return FALSE;
}

typedef struct {
    int rank;
    char *canonical;
    const char *type;
    gboolean blocked;
} CloudFileScope;

static void
cloudfile_scope_free (gpointer data)
{
    CloudFileScope *scope = data;
    free (scope->canonical);
    g_free (scope);
}

static gint
cloudfile_scope_compare (gconstpointer a, gconstpointer b)
{
    const CloudFileScope *one = *(CloudFileScope * const *)a;
    const CloudFileScope *two = *(CloudFileScope * const *)b;
    return one->rank != two->rank ? one->rank - two->rank : strcmp (one->canonical, two->canonical);
}

static gboolean
scope_identifier (json_t *value)
{
    if (!json_is_string (value))
        return FALSE;
    const char *text = json_string_value (value);
    return json_string_length (value) == strlen (text) && *text &&
           g_utf8_validate (text, -1, NULL) && g_utf8_strlen (text, -1) <= 255;
}

static gboolean
scope_is_exact (SeafDBRow *row, void *data)
{
    /* Hash lookup never substitutes for comparing the complete scope. */
    CloudFileScope *scope = data;
    if (g_strcmp0 (seaf_db_row_get_column_text (row, 0), scope->canonical) == 0)
        scope->blocked = TRUE;
    return TRUE;
}

static int
cloudfile_check_barriers (SeafDBTrans *trans, const char *repo_id, const char *scopes_json,
                         CcnetUserManager *user_mgr, const char *native_username)
{
    json_t *root = NULL, *scopes = NULL;
    GPtrArray *ordered = g_ptr_array_new_with_free_func (cloudfile_scope_free);
    char *database = NULL;
    int result = -1;
    gboolean has_user = FALSE, has_repo = FALSE;
    const char *user_id = NULL;
    if (strlen (scopes_json) > 16384)
        goto out;
    root = json_loads (scopes_json, JSON_REJECT_DUPLICATES, NULL);
    scopes = json_is_object (root) ? json_object_get (root, "scopes") : root;
    if (!json_is_array (scopes) || json_array_size (scopes) < 2 || json_array_size (scopes) > 16)
        goto out;
    for (size_t i = 0; i < json_array_size (scopes); ++i) {
        json_t *value = json_array_get (scopes, i);
        const char *key;
        json_t *field;
        if (!json_is_object (value))
            goto out;
        json_object_foreach (value, key, field) {
            if ((strcmp (key, "type") && strcmp (key, "provider") &&
                 strcmp (key, "external_id") && strcmp (key, "namespace")) || !scope_identifier (field))
                goto out;
        }
        if (!scope_identifier (json_object_get (value, "type")) ||
            !scope_identifier (json_object_get (value, "provider")) ||
            !scope_identifier (json_object_get (value, "external_id")))
            goto out;
        const char *type = json_string_value (json_object_get (value, "type"));
        int rank;
        if (strcmp (type, "provider") == 0)
            rank = 0;
        else if (strcmp (type, "user") == 0 || strcmp (type, "subject") == 0) {
            rank = 1;
            if (strcmp (type, "subject") == 0 && !json_object_get (value, "namespace"))
                goto out;
            has_user |= strcmp (type, "user") == 0;
            if (strcmp (type, "user") == 0) {
                const char *candidate = json_string_value (json_object_get (value, "external_id"));
                if (user_id && strcmp (user_id, candidate))
                    goto out;
                user_id = candidate;
            }
        } else if (strcmp (type, "repo") == 0) {
            rank = 2;
            /* This primitive publishes exactly one non-virtual repository. */
            if (strcmp (json_string_value (json_object_get (value, "external_id")), repo_id) != 0)
                goto out;
            has_repo = TRUE;
        } else
            goto out;
        CloudFileScope *scope = g_new0 (CloudFileScope, 1);
        scope->rank = rank;
        scope->type = type;
        scope->canonical = json_dumps (value, JSON_COMPACT | JSON_SORT_KEYS);
        if (!scope->canonical) {
            g_free (scope);
            goto out;
        }
        g_ptr_array_add (ordered, scope);
    }
    if (!has_user || !has_repo)
        goto out;
    g_ptr_array_sort (ordered, cloudfile_scope_compare);
    /* One total acquisition budget, matching Hub's monotonic deadline.
     * Never multiply five seconds by the number of requested scopes. */
    gint64 scope_deadline = g_get_monotonic_time () + 5 * G_TIME_SPAN_SECOND;
    if (seaf_db_trans_foreach_selected_row (trans, "SELECT DATABASE()", get_gc_id,
                                           &database, 0) != 1 || !database || strchr (database, '\n'))
        goto out;
    for (guint i = 0; i < ordered->len; ++i) {
        CloudFileScope *scope = g_ptr_array_index (ordered, i);
        if (i > 0 && strcmp (scope->canonical, ((CloudFileScope *)g_ptr_array_index (ordered, i-1))->canonical) == 0)
            continue;
        char *text = g_strdup_printf ("%s\n%s", database, scope->canonical);
        char *digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, text, -1);
        char *name = g_strdup_printf ("cf.auth.%.56s", digest);
        gint64 remaining = scope_deadline - g_get_monotonic_time ();
        int timeout = remaining > 0 ? (int)(remaining / G_TIME_SPAN_SECOND) : 0;
        int locked = seaf_db_trans_acquire_scope_lock (trans, name, timeout);
        g_free (name);
        g_free (digest);
        g_free (text);
        if (locked < 0)
            goto out;
    }
    /* Current/locking reads, after all authority locks and before GC/Branch.
     * JobStore establishes and clears barriers under these exact SQL locks. */
    for (guint i = 0; i < ordered->len; ++i) {
        CloudFileScope *scope = g_ptr_array_index (ordered, i);
        char *digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, scope->canonical, -1);
        int rows = seaf_db_trans_foreach_selected_row (
            trans, "SELECT scope_id FROM cf_background_job WHERE scope_type=? AND scope_hash=? AND barrier_active=1 FOR UPDATE",
            scope_is_exact, scope, 2, "string", scope->type, "string", digest);
        g_free (digest);
        if (rows < 0 || scope->blocked)
            goto out;
    }
    if (ccnet_user_manager_lock_active_account (user_mgr, trans, native_username) < 0 ||
        ccnet_user_manager_lock_business_identity (user_mgr, trans, native_username, user_id) < 0)
        goto out;
    result = 0;
out:
    g_free (database);
    g_ptr_array_free (ordered, TRUE);
    if (root)
        json_decref (root);
    return result;
}

/* Caller-supplied context is an expectation, never an authorization grant.
 * SQL user/provider locks are already held. Refresh begin/publish share them,
 * so no managed generation can replace the checked value until SQL commit.
 * Ordinary CE entry points and legacy barrier-only calls remain unchanged. */
static int
cloudfile_check_context (SeafBranchManager *mgr, const char *conditions, json_t **snapshot)
{
    int result = -1;
    json_t *root = json_loads (conditions, JSON_REJECT_DUPLICATES, NULL);
    json_t *context = json_is_object (root) ? json_object_get (root, "context") : NULL;
    redisContext *client = NULL;
    redisReply *reply = NULL;
    char *host = NULL, *password = NULL, *prefix = NULL, *encoded = NULL, *digest = NULL, *key = NULL;
    if (json_is_array (root)) { result = 0; goto out; }
    if (!json_is_object (context) || json_object_size (context) != 3)
        goto out;
    json_t *provider = json_object_get (context, "provider");
    json_t *user = json_object_get (context, "userId");
    json_t *epoch = json_object_get (context, "epoch");
    if (!scope_identifier (provider) || !scope_identifier (user) || !json_is_string (epoch))
        goto out;
    const char *generation = json_string_value (epoch);
    if (json_string_length (epoch) != 32 || strspn (generation, "0123456789abcdef") != 32)
        goto out;
    gboolean found_user = FALSE, found_provider = FALSE;
    json_t *scopes = json_object_get (root, "scopes");
    for (size_t i = 0; i < json_array_size (scopes); ++i) {
        json_t *scope = json_array_get (scopes, i);
        const char *type = json_string_value (json_object_get (scope, "type"));
        const char *source = json_string_value (json_object_get (scope, "provider"));
        const char *external = json_string_value (json_object_get (scope, "external_id"));
        if (g_strcmp0 (type, "user") == 0) {
            if (g_strcmp0 (source, json_string_value (provider)) ||
                g_strcmp0 (external, json_string_value (user))) goto out;
            found_user = TRUE;
        }
        if (g_strcmp0 (type, "provider") == 0 &&
            g_strcmp0 (source, json_string_value (provider)) == 0 &&
            g_strcmp0 (external, json_string_value (provider)) == 0)
            found_provider = TRUE;
    }
    if (!found_user || !found_provider) goto out;
    GKeyFile *config = mgr->seaf->config;
    host = g_key_file_get_string (config, "cloudfile", "subject_redis_host", NULL);
    int port = g_key_file_get_integer (config, "cloudfile", "subject_redis_port", NULL);
    prefix = g_key_file_get_string (config, "cloudfile", "subject_redis_prefix", NULL);
    password = g_key_file_get_string (config, "cloudfile", "subject_redis_password", NULL);
    if (!host || !*host || port < 1 || port > 65535 || !prefix || !*prefix || strlen (prefix) > 160)
        goto out;
    json_t *pair = json_array ();
    json_array_append (pair, provider);
    json_array_append (pair, user);
    encoded = json_dumps (pair, JSON_COMPACT);
    json_decref (pair);
    if (!encoded) goto out;
    digest = g_compute_checksum_for_string (G_CHECKSUM_SHA256, encoded, -1);
    key = g_strconcat (prefix, digest, NULL);
    struct timeval timeout = {1, 0};
    client = redisConnectWithTimeout (host, port, timeout);
    if (!client || client->err || redisSetTimeout (client, timeout) != REDIS_OK) goto out;
    if (password && *password) {
        reply = redisCommand (client, "AUTH %b", password, strlen (password));
        if (!reply || reply->type != REDIS_REPLY_STATUS || g_strcmp0 (reply->str, "OK")) goto out;
        freeReplyObject (reply); reply = NULL;
    }
    const char *script =
        "local raw=redis.call('GET',KEYS[1]); if not raw or #raw>1048576 then return 0 end; "
        "local ok,v=pcall(cjson.decode,raw); if not ok or type(v)~='table' then return 0 end; "
        "local t=redis.call('TIME'); local now=tonumber(t[1])+tonumber(t[2])/1000000; "
        "if v.userId~=ARGV[1] or v.context_epoch~=ARGV[2] or v.status~='ready' "
        "or type(v.expires_at)~='number' or type(v.fetched_at)~='number' "
        "or not(v.expires_at>now and v.expires_at-v.fetched_at>0 "
        "and v.expires_at-v.fetched_at<=1800 and v.fetched_at<=now+60) "
        "or redis.call('PTTL',KEYS[1])<=0 or redis.call('EXISTS',KEYS[2])~=0 "
        "or type(v.subject)~='table' or v.subject.userId~=ARGV[1] "
        "or v.subject.status~='active' or type(v.source_etag)~='string' "
        "or v.source_etag~=v.subject.etag then return 0 end; return raw";
    char *lease = g_strconcat (key, ":lease", NULL);
    reply = redisCommand (client, "EVAL %s 2 %s %s %s %s", script, key, lease,
                          json_string_value (user), generation);
    g_free (lease);
    /* A trusted delegation host must attach the verified token's immutable
     * service/jti/lifetime. The local RPC socket is the trust boundary; this
     * is not a public JWT verifier or permission grant. Recheck with Redis
     * server time on every context check, including each transfer chunk. */
    json_t *delegation = json_object_get (root, "user_delegation");
    if (delegation) {
        json_t *service = json_object_get (delegation, "service_id");
        json_t *token = json_object_get (delegation, "token_id");
        json_t *issued = json_object_get (delegation, "issued_at");
        json_t *expires = json_object_get (delegation, "expires_at");
        json_t *transfer = json_object_get (root, "read_transfer_expires_at");
        if (!json_is_object (delegation) || json_object_size (delegation) != 4 ||
            !scope_identifier (service) || !scope_identifier (token) ||
            json_string_length (token) > 128 || !json_is_integer (issued) ||
            !json_is_integer (expires) || json_integer_value (issued) < 0 ||
            json_integer_value (expires) <= json_integer_value (issued) ||
            json_integer_value (expires) - json_integer_value (issued) > 60)
            goto out;
        /* Only the native ticket manager constructs this field AFTER a
         * successful one-time consumption. Issuance and writes reject it.
         * Original JWT lifetime stays immutable; revocation remains checked. */
        if (transfer && (!json_is_integer (transfer) ||
            json_integer_value (transfer) <= json_integer_value (issued) ||
            json_integer_value (transfer) - json_integer_value (expires) > 300))
            goto out;
        char *revocation_prefix = g_key_file_get_string (config, "cloudfile",
            "delegation_revocation_prefix", NULL);
        if (!revocation_prefix || strlen (revocation_prefix) > 128 ||
            !g_str_has_prefix (revocation_prefix, "cf:") ||
            !g_str_has_suffix (revocation_prefix, ":")) {
            g_free (revocation_prefix);
            goto out;
        }
        json_t *identity = json_array ();
        json_array_append (identity, service);
        json_array_append (identity, token);
        char *serialized = json_dumps (identity, JSON_COMPACT);
        json_decref (identity);
        if (!serialized) { g_free (revocation_prefix); goto out; }
        char *revocation_hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, serialized, -1);
        char *revocation_key = g_strconcat (revocation_prefix, revocation_hash, NULL);
        free (serialized); g_free (revocation_hash); g_free (revocation_prefix);
        redisReply *active = redisCommand (client,
            "EVAL %s 1 %s %lld %lld",
            "local t=redis.call('TIME'); local now=tonumber(t[1])+tonumber(t[2])/1000000; "
            "if tonumber(ARGV[1])>now+30 or tonumber(ARGV[2])<=now "
            "or redis.call('EXISTS',KEYS[1])~=0 then return 0 end; return 1",
            revocation_key, (long long)json_integer_value (issued),
            (long long)json_integer_value (transfer ? transfer : expires));
        g_free (revocation_key);
        gboolean allowed = active && active->type == REDIS_REPLY_INTEGER && active->integer == 1;
        if (active) freeReplyObject (active);
        if (!allowed) goto out;
    }
    if (reply && reply->type == REDIS_REPLY_STRING && reply->len <= 1048576) {
        json_t *value = json_loadb (reply->str, reply->len, JSON_REJECT_DUPLICATES, NULL);
        if (json_is_object (value)) {
            if (snapshot) *snapshot = value; else json_decref (value);
            result = 0;
        } else if (value) json_decref (value);
    }
out:
    if (reply) freeReplyObject (reply);
    if (client) redisFree (client);
    if (root) json_decref (root);
    g_free (host); g_free (password); g_free (prefix);
    free (encoded); g_free (digest); g_free (key);
    return result;
}

static gboolean
cloudfile_repo_status (SeafDBRow *row, void *data)
{
    gboolean *writable = data;
    const char *status = seaf_db_row_get_column_text (row, 0);
    *writable = status && strcmp (status, "0") == 0;
    return FALSE;
}

static gboolean
cloudfile_repo_read_status (SeafDBRow *row, void *data)
{
    gboolean *readable = data;
    const char *status = seaf_db_row_get_column_text (row, 0);
    *readable = status && (!strcmp (status, "0") || !strcmp (status, "1"));
    return FALSE;
}

static int
cloudfile_check_repo_mode (SeafDBTrans *trans, const char *repo_id, gboolean read)
{
    gboolean error = FALSE, writable = FALSE;
    /* These native rows share the branch transaction. Suspension, deletion or
     * conversion after indexing cannot bypass the final current/locking read.
     * Row order inside the repository scope: Repo -> RepoInfo -> VirtualRepo,
     * then GCID -> Branch. No Hub callback while these rows are held. */
    if (!seaf_db_trans_check_for_existence (trans,
            "SELECT repo_id FROM Repo WHERE repo_id=? FOR UPDATE", &error,
            1, "string", repo_id) || error)
        return -1;
    if (seaf_db_trans_foreach_selected_row (trans,
            "SELECT status FROM RepoInfo WHERE repo_id=? FOR UPDATE",
            read ? cloudfile_repo_read_status : cloudfile_repo_status,
            &writable, 1, "string", repo_id) != 1 || !writable)
        return -1;
    if (seaf_db_trans_check_for_existence (trans,
            "SELECT repo_id FROM VirtualRepo WHERE repo_id=? FOR UPDATE", &error,
            1, "string", repo_id) || error)
        return -1;
    return 0;
}

static int
cloudfile_check_repo (SeafDBTrans *trans, const char *repo_id)
{
    return cloudfile_check_repo_mode (trans, repo_id, FALSE);
}

typedef struct {
    const char *username;
    gboolean valid;
    int permission;
    GArray *groups;
} CloudFileQualification;

static gboolean
cloudfile_owner_row (SeafDBRow *row, void *data)
{
    CloudFileQualification *q = data;
    const char *owner = seaf_db_row_get_column_text (row, 0);
    if (!owner || !*owner) q->valid = FALSE;
    else if (strcmp (owner, q->username) == 0) q->permission = 2;
    return TRUE;
}

static gboolean
cloudfile_share_row (SeafDBRow *row, void *data)
{
    CloudFileQualification *q = data;
    const char *permission = seaf_db_row_get_column_text (row, 0);
    const char *target = seaf_db_row_get_column_text (row, 1);
    if (target && strcmp (target, q->username) == 0) {
        if (g_strcmp0 (permission, "rw") == 0) q->permission = MAX (q->permission, 2);
        else if (g_strcmp0 (permission, "r") == 0) q->permission = MAX (q->permission, 1);
        else q->valid = FALSE;
    } else q->valid = FALSE;
    return TRUE;
}

static gboolean
cloudfile_group_member_row (SeafDBRow *row, void *data)
{
    CloudFileQualification *q = data;
    const char *raw_id = seaf_db_row_get_column_text (row, 0);
    char *end = NULL;
    gint64 parsed = raw_id ? g_ascii_strtoll (raw_id, &end, 10) : 0;
    int id = parsed > 0 && parsed <= G_MAXINT && end && !*end ? (int)parsed : 0;
    const char *username = seaf_db_row_get_column_text (row, 1);
    if (id <= 0 || g_strcmp0 (username, q->username) || q->groups->len >= 4096)
        q->valid = FALSE;
    else g_array_append_val (q->groups, id);
    return q->valid;
}

typedef struct {
    GHashTable *ids, *parents, *paths;
    gboolean valid, collect;
} CloudFileNativeGroups;

static int
cloudfile_native_integer (const char *text, int minimum)
{
    char *end = NULL;
    gint64 number = text ? g_ascii_strtoll (text, &end, 10) : -2;
    char canonical[32];
    g_snprintf (canonical, sizeof canonical, "%" G_GINT64_FORMAT, number);
    return !text || !end || *end || number < minimum || number > G_MAXINT ||
           strcmp (text, canonical) ? -2 : (int)number;
}

static gboolean
cloudfile_group_parent_row (SeafDBRow *row, void *data)
{
    CloudFileNativeGroups *g = data;
    int id = cloudfile_native_integer (seaf_db_row_get_column_text (row, 0), 1);
    int parent = cloudfile_native_integer (seaf_db_row_get_column_text (row, 1), -1);
    if (id < 1 || parent < -1 || !g_hash_table_contains (g->ids, GINT_TO_POINTER (id)) ||
        g_hash_table_contains (g->parents, GINT_TO_POINTER (id))) g->valid = FALSE;
    else {
        int *value = g_new (int, 1); *value = parent;
        g_hash_table_insert (g->parents, GINT_TO_POINTER (id), value);
    }
    return g->valid;
}

static gboolean
cloudfile_structure_row (SeafDBRow *row, void *data)
{
    CloudFileNativeGroups *g = data;
    int id = cloudfile_native_integer (seaf_db_row_get_column_text (row, 0), 1);
    const char *path = seaf_db_row_get_column_text (row, 1);
    if (id < 1 || !path || !*path || strlen (path) > 1024 ||
        !g_hash_table_contains (g->ids, GINT_TO_POINTER (id)) ||
        g_hash_table_contains (g->paths, GINT_TO_POINTER (id))) { g->valid = FALSE; return FALSE; }
    char **tokens = g_strsplit (path, ", ", -1);
    guint length = g_strv_length (tokens);
    int last = -2;
    if (!length || length > 128) g->valid = FALSE;
    for (guint i = 0; g->valid && i < length; ++i) {
        last = cloudfile_native_integer (tokens[i], 1);
        if (last < 1 || (!g_hash_table_contains (g->ids, GINT_TO_POINTER (last)) &&
                        (!g->collect || g_hash_table_size (g->ids) >= 4096))) g->valid = FALSE;
        else if (g->collect) g_hash_table_add (g->ids, GINT_TO_POINTER (last));
    }
    if (last != id) g->valid = FALSE;
    g_strfreev (tokens);
    if (g->valid) g_hash_table_insert (g->paths, GINT_TO_POINTER (id), g_strdup (path));
    return g->valid;
}

static char *
cloudfile_group_query (const char *prefix, GHashTable *ids)
{
    GString *query = g_string_new (prefix);
    GHashTableIter iter; gpointer id; gboolean first = TRUE;
    g_hash_table_iter_init (&iter, ids);
    while (g_hash_table_iter_next (&iter, &id, NULL)) {
        g_string_append_printf (query, "%s%d", first ? "" : ",", GPOINTER_TO_INT (id)); first = FALSE;
    }
    g_string_append (query, ") ORDER BY group_id LIMIT 4097 FOR UPDATE");
    return g_string_free (query, FALSE);
}

static gboolean
cloudfile_innodb_row (SeafDBRow *row, void *data)
{
    gboolean *valid = data;
    *valid = g_strcmp0 (seaf_db_row_get_column_text (row, 0), "InnoDB") == 0;
    return FALSE;
}

static gboolean
cloudfile_qualification_isolation (SeafDBRow *row, void *data)
{
    gboolean *valid = data;
    const char *isolation = seaf_db_row_get_column_text (row, 0);
    *valid = g_strcmp0 (isolation, "REPEATABLE-READ") == 0 ||
             g_strcmp0 (isolation, "SERIALIZABLE") == 0;
    return FALSE;
}

static char *
cloudfile_quote_identifier (const char *value)
{
    if (!value || !*value || !g_utf8_validate (value, -1, NULL) || g_utf8_strlen (value, -1) > 64)
        return NULL;
    GString *quoted = g_string_new ("`");
    for (const char *p = value; *p; ++p) {
        if (*p == '`') g_string_append_c (quoted, '`');
        g_string_append_c (quoted, *p);
    }
    g_string_append_c (quoted, '`');
    return g_string_free (quoted, FALSE);
}

/* Current, locking CE qualification. No cached RPC/group membership is trusted.
 * Returns 0/1/2 for unqualified/read/write, -1 for ambiguous/unavailable state.
 * Caller owns authority scopes, native account and repository locks. */
static int
cloudfile_library_qualification (SeafBranchManager *mgr, SeafDBTrans *trans,
                                const char *repo, const char *username)
{
    CloudFileQualification q = {username, TRUE, 0, NULL};
    int result = -1;
    char *schema = NULL, *group_table = NULL, *sql = NULL;
    GHashTable *all = NULL, *parents = NULL, *structures = NULL;
    const char *native_schema = seaf_db_mysql_shared_database (mgr->seaf->db, mgr->seaf->ccnet_db);
    const char *table = g_getenv ("SEAFILE_MYSQL_DB_GROUP_TABLE_NAME");
    if (!table || !*table) table = "Group";
    /* Lock metadata too: nontransactional legacy tables cannot prove authority. */
    const char *tables[] = {"RepoOwner", "SharedRepo", "RepoGroup", "InnerPubRepo"};
    gboolean isolation = FALSE;
    int isolation_rows = seaf_db_trans_foreach_selected_row (trans,
        "SELECT @@transaction_isolation", cloudfile_qualification_isolation, &isolation, 0);
    if (isolation_rows < 0)
        isolation_rows = seaf_db_trans_foreach_selected_row (trans,
            "SELECT @@tx_isolation", cloudfile_qualification_isolation, &isolation, 0);
    /* Missing personal shares/memberships are also authority: protect absence
     * against a concurrent restrictive share insertion, not just existing rows. */
    if (isolation_rows != 1 || !isolation) goto out;
    for (guint i = 0; i < G_N_ELEMENTS (tables); ++i) {
        gboolean engine = FALSE;
        if (seaf_db_trans_foreach_selected_row (trans,
            "SELECT ENGINE FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name=?",
            cloudfile_innodb_row, &engine, 1, "string", tables[i]) != 1 || !engine) goto out;
    }
    if (seaf_db_trans_foreach_selected_row (trans,
        "SELECT owner_id FROM RepoOwner WHERE repo_id=? FOR UPDATE", cloudfile_owner_row,
        &q, 1, "string", repo) != 1 || !q.valid) goto out;
    if (q.permission) { result = q.permission; goto out; }
    int rows = seaf_db_trans_foreach_selected_row (trans,
        "SELECT permission,to_email FROM SharedRepo WHERE repo_id=? AND to_email=? LIMIT 2 FOR UPDATE",
        cloudfile_share_row, &q, 2, "string", repo, "string", username);
    if (rows < 0 || rows > 1 || !q.valid) goto out;
    if (rows) { result = q.permission; goto out; } /* Personal share dominates groups. */
    schema = cloudfile_quote_identifier (native_schema);
    group_table = cloudfile_quote_identifier (table);
    if (!schema || !group_table) goto out;
    const char *native_tables[] = {"GroupUser", "GroupStructure", table};
    for (guint i = 0; i < G_N_ELEMENTS (native_tables); ++i) {
        gboolean engine = FALSE;
        if (seaf_db_trans_foreach_selected_row (trans,
            "SELECT ENGINE FROM information_schema.tables WHERE table_schema=? AND table_name=?",
            cloudfile_innodb_row, &engine, 2, "string", native_schema, "string", native_tables[i]) != 1 || !engine) goto out;
    }
    q.groups = g_array_new (FALSE, FALSE, sizeof (int));
    sql = g_strdup_printf ("SELECT group_id,user_name FROM %s.GroupUser WHERE user_name=? ORDER BY group_id LIMIT 4097 FOR UPDATE", schema);
    rows = seaf_db_trans_foreach_selected_row (trans, sql, cloudfile_group_member_row, &q, 1, "string", username);
    g_free (sql); sql = NULL;
    if (rows < 0 || !q.valid) goto out;
    all = g_hash_table_new (g_direct_hash, g_direct_equal);
    parents = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    structures = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, g_free);
    for (guint i = 0; i < q.groups->len; ++i)
        g_hash_table_add (all, GINT_TO_POINTER (g_array_index (q.groups, int, i)));
    CloudFileNativeGroups native = {all, parents, structures, TRUE, TRUE};
    /* Four indexed batches instead of one SQL round trip per ancestor. Parent
     * chains are validated from row-locked data entirely in bounded memory. */
    if (g_hash_table_size (all)) {
        char *prefix = g_strdup_printf ("SELECT group_id,path FROM %s.GroupStructure WHERE group_id IN (", schema);
        sql = cloudfile_group_query (prefix, all); g_free (prefix);
        rows = seaf_db_trans_foreach_selected_row (trans, sql, cloudfile_structure_row, &native, 0);
        g_free (sql); sql = NULL;
        if (rows < 0 || !native.valid) goto out;
        prefix = g_strdup_printf ("SELECT group_id,parent_group_id FROM %s.%s WHERE group_id IN (", schema, group_table);
        sql = cloudfile_group_query (prefix, all); g_free (prefix);
        rows = seaf_db_trans_foreach_selected_row (trans, sql, cloudfile_group_parent_row, &native, 0);
        g_free (sql); sql = NULL;
        if (rows != (int)g_hash_table_size (all) || !native.valid) goto out;
        g_hash_table_remove_all (structures);
        native.collect = FALSE;
        prefix = g_strdup_printf ("SELECT group_id,path FROM %s.GroupStructure WHERE group_id IN (", schema);
        sql = cloudfile_group_query (prefix, all); g_free (prefix);
        rows = seaf_db_trans_foreach_selected_row (trans, sql, cloudfile_structure_row, &native, 0);
        g_free (sql); sql = NULL;
        if (rows < 0 || !native.valid) goto out;
    }
    for (guint i = 0; i < q.groups->len; ++i) {
        GArray *chain = g_array_new (FALSE, FALSE, sizeof (int));
        int id = g_array_index (q.groups, int, i), parent = -2;
        gboolean valid = TRUE;
        while (id > 0) {
            if (chain->len >= 128 || (!g_hash_table_contains (all, GINT_TO_POINTER (id)) &&
                                     g_hash_table_size (all) >= 4096)) { valid = FALSE; break; }
            for (guint j = 0; j < chain->len; ++j)
                if (g_array_index (chain, int, j) == id) valid = FALSE;
            if (!valid) break;
            g_array_append_val (chain, id);
            g_hash_table_add (all, GINT_TO_POINTER (id));
            int *cached = g_hash_table_lookup (parents, GINT_TO_POINTER (id));
            if (!cached) { valid = FALSE; break; }
            parent = *cached;
            id = parent;
        }
        if (valid && (chain->len > 1 || parent == -1)) {
            if (parent != -1) valid = FALSE;
            GString *expected = g_string_new ("");
            for (guint j = chain->len; valid && j > 0; --j) {
                id = g_array_index (chain, int, j - 1);
                if (*expected->str) g_string_append (expected, ", ");
                g_string_append_printf (expected, "%d", id);
                if (g_strcmp0 (expected->str, g_hash_table_lookup (structures, GINT_TO_POINTER (id)))) valid = FALSE;
            }
            g_string_free (expected, TRUE);
        }
        g_array_free (chain, TRUE);
        if (!valid) goto out;
    }
    if (g_hash_table_size (all)) {
        GString *query = g_string_new ("SELECT permission,? FROM RepoGroup WHERE repo_id=? AND group_id IN (");
        GHashTableIter iter; gpointer id; gboolean first = TRUE;
        g_hash_table_iter_init (&iter, all);
        while (g_hash_table_iter_next (&iter, &id, NULL)) {
            g_string_append_printf (query, "%s%d", first ? "" : ",", GPOINTER_TO_INT (id)); first = FALSE;
        }
        g_string_append (query, ") ORDER BY group_id LIMIT 4097 FOR UPDATE");
        rows = seaf_db_trans_foreach_selected_row (trans, query->str, cloudfile_share_row, &q,
            2, "string", username, "string", repo);
        g_string_free (query, TRUE);
        if (rows < 0 || rows > 4096 || !q.valid) goto out;
        if (q.permission) { result = q.permission; goto out; }
    }
    if (!mgr->seaf->cloud_mode) {
        rows = seaf_db_trans_foreach_selected_row (trans,
            "SELECT permission,? FROM InnerPubRepo WHERE repo_id=? LIMIT 2 FOR UPDATE",
            cloudfile_share_row, &q, 2, "string", username, "string", repo);
        if (rows < 0 || rows > 1 || !q.valid) goto out;
    }
    result = q.permission;
out:
    /* Check engines again after locking reads have pinned the table metadata.
     * Information_schema before a read alone cannot exclude an ALTER race. */
    for (guint i = 0; result >= 0 && i < G_N_ELEMENTS (tables); ++i) {
        gboolean engine = FALSE;
        if (seaf_db_trans_foreach_selected_row (trans,
            "SELECT ENGINE FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name=?",
            cloudfile_innodb_row, &engine, 1, "string", tables[i]) != 1 || !engine) result = -1;
    }
    const char *final_native_tables[] = {"GroupUser", "GroupStructure", table};
    for (guint i = 0; result >= 0 && q.groups && i < G_N_ELEMENTS (final_native_tables); ++i) {
        gboolean engine = FALSE;
        if (seaf_db_trans_foreach_selected_row (trans,
            "SELECT ENGINE FROM information_schema.tables WHERE table_schema=? AND table_name=?",
            cloudfile_innodb_row, &engine, 2, "string", native_schema,
            "string", final_native_tables[i]) != 1 || !engine) result = -1;
    }
    if (q.groups) g_array_free (q.groups, TRUE);
    if (all) g_hash_table_destroy (all);
    if (parents) g_hash_table_destroy (parents);
    if (structures) g_hash_table_destroy (structures);
    g_free (schema); g_free (group_table); g_free (sql);
    return result;
}

int
seaf_branch_manager_check_read_with_barriers (SeafBranchManager *mgr,
    SeafDBTrans *trans, const char *repo_id, const char *path, int kind,
    const char *conditions, const char *native_username)
{
    int result = -2;
    json_t *root = NULL, *snapshot = NULL;
    if (!mgr || !trans || !repo_id || !path || !conditions || !native_username ||
        strlen (conditions) > 16384)
        return -2;
    root = json_loads (conditions, JSON_REJECT_DUPLICATES, NULL);
    json_t *context = json_object_get (root, "context");
    json_t *target = json_object_get (root, "path");
    if (!json_is_object (root) || !json_is_object (context) || !json_is_string (target) ||
        json_string_length (target) != strlen (path) || strcmp (json_string_value (target), path))
        goto out;
    if (cloudfile_check_barriers (trans, repo_id, conditions, mgr->seaf->user_mgr, native_username) < 0 ||
        cloudfile_check_repo_mode (trans, repo_id, TRUE) < 0)
        goto out;
    int qualification = cloudfile_library_qualification (mgr, trans, repo_id, native_username);
    if (qualification <= 0 || cloudfile_check_context (mgr, conditions, &snapshot) < 0 ||
        cf_policy_check_read (trans, repo_id, path,
            json_string_value (json_object_get (context, "provider")),
            json_string_value (json_object_get (context, "userId")), snapshot, qualification, kind) < 0 ||
        cloudfile_check_context (mgr, conditions, NULL) < 0)
        goto out;
    result = 0;
out:
    if (snapshot) json_decref (snapshot);
    if (root) json_decref (root);
    return result;
}

/* Optional only for non-OIDC trusted callers. An OIDC host must include this
 * server-derived reference and its provider scope; omission is not logout proof. */
static const char *
cloudfile_oidc_identifier (json_t *object, const char *name, size_t maximum)
{
    json_t *value = json_object_get (object, name);
    const char *text = json_string_value (value);
    return text && json_string_length (value) == strlen (text) &&
        strlen (text) <= maximum ? text : NULL;
}

static int
cloudfile_check_oidc_schema (SeafDBTrans *trans)
{
    gboolean error = FALSE;
    const char *versions[] = {"012_oidc_sessions", "013_oidc_logout_fences", "014_oidc_scope_expiry"};
    for (int i = 0; i < 3; ++i) {
        if (!seaf_db_trans_check_for_existence (trans,
            "SELECT version FROM cf_schema_migration WHERE version=? AND state='applied' AND step=1 FOR UPDATE",
            &error, 1, "string", versions[i]) || error) return -1;
    }
    const char *tables[] = {"cf_oidc_session", "cf_oidc_logout_fence"};
    for (int i = 0; i < 2; ++i) {
        if (!seaf_db_trans_check_for_existence (trans,
            "SELECT table_name FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name=? AND engine='InnoDB'",
            &error, 1, "string", tables[i]) || error) return -1;
    }
    /* Verify exact non-prefix index axes, not merely an index bearing the
     * expected name. Hub's require_current additionally checks all checksums
     * and migration structures; this native gate does not replace that. */
    const char *checks[] = {
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_oidc_session' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=2 AND SUM(non_unique=0 AND sub_part IS NULL AND ((seq_in_index=1 AND column_name='scope_hash') OR (seq_in_index=2 AND column_name='session_key')))=2",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_oidc_logout_fence' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=3 AND SUM(non_unique=0 AND sub_part IS NULL AND ((seq_in_index=1 AND column_name='scope_hash') OR (seq_in_index=2 AND column_name='target_type') OR (seq_in_index=3 AND column_name='target_hash')))=3",
        "SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_oidc_session' AND index_name='oidc_scope_expiry' GROUP BY index_name HAVING COUNT(*)=3 AND SUM(non_unique=1 AND sub_part IS NULL AND ((seq_in_index=1 AND column_name='scope_hash') OR (seq_in_index=2 AND column_name='expires_at') OR (seq_in_index=3 AND column_name='session_key')))=3"
    };
    for (int i = 0; i < 3; ++i)
        if (!seaf_db_trans_check_for_existence (trans, checks[i], &error, 0) || error) return -1;
    return 0;
}

static int
cloudfile_check_oidc_reference (SeafBranchManager *mgr, SeafDBTrans *trans, json_t *root)
{
    json_t *reference = json_object_get (root, "oidc_session");
    if (!reference) return 0;
    if (!json_is_object (reference) || json_object_size (reference) != 5) return -1;
    const char *scope = cloudfile_oidc_identifier (reference, "scope_hash", 64);
    const char *key = cloudfile_oidc_identifier (reference, "session_key", 32);
    const char *subject = cloudfile_oidc_identifier (reference, "subject_hash", 64);
    json_t *sid_value = json_object_get (reference, "sid_hash");
    const char *sid = json_is_null (sid_value) ? NULL : cloudfile_oidc_identifier (reference, "sid_hash", 64);
    json_t *issued = json_object_get (reference, "authenticated_at");
    if (!scope || strlen (scope) != 64 || strspn (scope, "0123456789abcdef") != 64 ||
        !key || strlen (key) != 32 || strspn (key, "0123456789abcdefghijklmnopqrstuvwxyz") != 32 ||
        !subject || strlen (subject) != 64 || strspn (subject, "0123456789abcdef") != 64 ||
        (!json_is_null (sid_value) && (!sid || strlen (sid) != 64 || strspn (sid, "0123456789abcdef") != 64)) ||
        !json_is_integer (issued) || json_integer_value (issued) < 0) return -1;
    char provider[33];
    g_snprintf (provider, sizeof provider, "cf_oidc_%.24s", scope);
    gboolean locked_scope = FALSE;
    json_t *scopes = json_object_get (root, "scopes");
    for (size_t i = 0; i < json_array_size (scopes); ++i) {
        json_t *candidate = json_array_get (scopes, i);
        if (!g_strcmp0 (json_string_value (json_object_get (candidate, "type")), "provider") &&
            !g_strcmp0 (json_string_value (json_object_get (candidate, "provider")), provider) &&
            !g_strcmp0 (json_string_value (json_object_get (candidate, "external_id")), provider)) locked_scope = TRUE;
    }
    if (!locked_scope || cloudfile_check_oidc_schema (trans) < 0) return -1;
    gboolean error = FALSE;
    if (!seaf_db_trans_check_for_existence (trans,
        "SELECT session_key FROM cf_oidc_session WHERE scope_hash=? AND session_key=? "
        "AND subject_hash=? AND COALESCE(sid_hash,'')=? AND authenticated_at=? AND expires_at>UTC_TIMESTAMP(6) FOR UPDATE",
        &error, 5, "string", scope, "string", key, "string", subject, "string", sid ? sid : "",
        "int64", (gint64)json_integer_value (issued)) || error) return -1;
    const char *types[] = {"subject", "sid"};
    const char *targets[] = {subject, sid};
    for (int i = 0; i < 2; ++i) {
        if (!targets[i]) continue;
        if (seaf_db_trans_check_for_existence (trans,
            "SELECT cutoff_at FROM cf_oidc_logout_fence WHERE scope_hash=? AND target_type=? "
            "AND target_hash=? AND cutoff_at>=? FOR UPDATE", &error,
            4, "string", scope, "string", types[i], "string", targets[i],
            "int64", (gint64)json_integer_value (issued)) || error) return -1;
    }
    /* Locking read, not an RR snapshot: local flush must invalidate a transfer
     * even when subsequent index cleanup fails. Signed reference verification
     * remains the trusted Hub's responsibility before ticket issuance. */
    return ccnet_user_manager_lock_live_session (mgr->seaf->user_mgr, trans, key);
}

static int
cloudfile_check_local_reference (SeafBranchManager *mgr, SeafDBTrans *trans,
    const char *repo_id, const char *path, const char *object_id, int kind,
    const char *conditions, const char *native_username, json_t *root)
{
    if (!json_object_get (root, "local_session")) return 0;
    json_t *subject = NULL;
    int qualification = cloudfile_library_qualification (mgr, trans, repo_id, native_username);
    if (qualification <= 0 || cloudfile_check_context (mgr, conditions, &subject) < 0) {
        if (subject) json_decref (subject);
        return -1;
    }
    /* Read-only libraries may still serve view sessions, never edit sessions.
     * CE qualification alone must not hide the native repository status. */
    if (cloudfile_check_repo_mode (trans, repo_id, FALSE) < 0)
        qualification = 1;
    int result = cf_policy_check_local_session (trans, repo_id, path, object_id,
        kind, root, subject, qualification);
    if (subject) json_decref (subject);
    return result;
}

int
seaf_branch_manager_check_read_target (SeafBranchManager *mgr, SeafDBTrans *trans,
    const char *repo_id, const char *path, int kind, const char *head_id,
    const char *object_id, const char *conditions, const char *native_username)
{
    char *current_head = NULL, *actual = NULL;
    SeafRepo *repo = NULL;
    SeafCommit *commit = NULL;
    GError *error = NULL;
    int result = -2;
    if (!head_id || !object_id || !is_object_id_valid (head_id) ||
        !is_object_id_valid (object_id) ||
        seaf_branch_manager_check_read_with_barriers (mgr, trans, repo_id, path,
            kind, conditions, native_username) < 0)
        return -2;
    json_t *reference_root = json_loads (conditions, JSON_REJECT_DUPLICATES, NULL);
    int reference_result = cloudfile_check_oidc_reference (mgr, trans, reference_root);
    if (reference_result == 0)
        reference_result = cloudfile_check_local_reference (mgr, trans, repo_id, path,
            object_id, kind, conditions, native_username, reference_root);
    if (reference_root) json_decref (reference_root);
    if (reference_result < 0) return -2;
    /* Pin the actual master row after repository/authority locks. A cached
     * repo->head must never prove that a ticket still targets this version. */
    if (seaf_db_trans_foreach_selected_row (trans,
            "SELECT commit_id FROM Branch WHERE repo_id=? AND name='master' FOR UPDATE",
            get_gc_id, &current_head, 1, "string", repo_id) != 1 ||
        g_strcmp0 (current_head, head_id))
        goto out;
    repo = seaf_repo_manager_get_repo (mgr->seaf->repo_mgr, repo_id);
    if (!repo || repo->virtual_info)
        goto out;
    commit = seaf_commit_manager_get_commit (mgr->seaf->commit_mgr,
        repo_id, repo->version, current_head);
    if (!commit)
        goto out;
    guint32 mode = 0;
    actual = seaf_fs_manager_path_to_obj_id (mgr->seaf->fs_mgr,
        repo->store_id, repo->version, commit->root_id, path, &mode, &error);
    if (error || g_strcmp0 (actual, object_id) ||
        (kind == CF_FILE && !S_ISREG (mode)) ||
        (kind == CF_DIRECTORY && !S_ISDIR (mode)) ||
        cloudfile_check_context (mgr, conditions, NULL) < 0)
        goto out;
    reference_root = json_loads (conditions, JSON_REJECT_DUPLICATES, NULL);
    reference_result = cloudfile_check_oidc_reference (mgr, trans, reference_root);
    if (reference_result == 0)
        reference_result = cloudfile_check_local_reference (mgr, trans, repo_id, path,
            object_id, kind, conditions, native_username, reference_root);
    if (reference_root) json_decref (reference_root);
    if (reference_result < 0) goto out;
    result = 0;
out:
    if (error) g_error_free (error);
    if (repo) seaf_repo_unref (repo);
    if (commit) seaf_commit_unref (commit);
    g_free (actual);
    g_free (current_head);
    return result;
}

/* The immutable commit is read while the matching Branch row is locked.
 * A no-content Checkin can release only the exact file that was checked out. */
static int
cloudfile_check_unchanged_file (SeafBranchManager *mgr, const char *repo_id,
                                const char *commit_id, const char *path,
                                const char *expected_file_id)
{
    SeafRepo *repo = NULL;
    SeafCommit *commit = NULL;
    char *actual = NULL;
    guint32 mode = 0;
    GError *error = NULL;
    int result = -1;
    if (!path || !expected_file_id || !is_object_id_valid (expected_file_id))
        return -1;
    repo = seaf_repo_manager_get_repo (mgr->seaf->repo_mgr, repo_id);
    if (!repo || repo->virtual_info || repo->status != REPO_STATUS_NORMAL)
        goto out;
    commit = seaf_commit_manager_get_commit (mgr->seaf->commit_mgr,
        repo_id, repo->version, commit_id);
    if (!commit) goto out;
    actual = seaf_fs_manager_path_to_obj_id (mgr->seaf->fs_mgr,
        repo->store_id, repo->version, commit->root_id, path, &mode, &error);
    if (!error && S_ISREG (mode) && g_strcmp0 (actual, expected_file_id) == 0)
        result = 0;
out:
    if (error) g_error_free (error);
    g_free (actual);
    if (commit) seaf_commit_unref (commit);
    if (repo) seaf_repo_unref (repo);
    return result;
}

static int
test_and_update_branch (SeafBranchManager *mgr,
                                            SeafBranch *branch,
                                            const char *old_commit_id,
                                            gboolean check_gc,
                                            const char *last_gc_id,
                                            const char *origin_repo_id,
                                            gboolean *gc_conflict,
                                            const char *scopes_json,
                                            const char *native_username)
{
    SeafDBTrans *trans;
    char *sql;
    char commit_id[41] = { 0 };
    char *gc_id = NULL;

    if (check_gc)
        *gc_conflict = FALSE;

    trans = seaf_db_begin_transaction (mgr->seaf->db);
    if (!trans)
        return -1;

    json_t *guard_conditions = scopes_json ? json_loads (scopes_json, JSON_REJECT_DUPLICATES, NULL) : NULL;
    gboolean enhanced = json_is_object (json_object_get (guard_conditions, "context"));
    if (guard_conditions) json_decref (guard_conditions);
    if (scopes_json && (cloudfile_check_barriers (trans, branch->repo_id, scopes_json,
                                               mgr->seaf->user_mgr, native_username) < 0 ||
                       cloudfile_check_repo (trans, branch->repo_id) < 0)) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -2;
    }
    /* Match policy mutation order: authority scopes, managed marker, Branch.
     * Enrollment rolls back with any denied/failed enhanced publication. */
    if (cf_policy_managed_guard_required (mgr->seaf->db, mgr->seaf->config) &&
        (enhanced ? cf_policy_enroll_managed_library (trans, branch->repo_id) :
         cf_policy_check_legacy_library (trans, branch->repo_id)) < 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -2;
    }

    if (check_gc) {
        sql = "SELECT gc_id FROM GCID WHERE repo_id = ? FOR UPDATE";
        if (!origin_repo_id) {
            if (seaf_db_trans_foreach_selected_row (trans, sql,
                                                    get_gc_id, &gc_id,
                                                    1, "string", branch->repo_id) < 0) {
                seaf_db_rollback (trans);
                seaf_db_trans_close (trans);
                return -1;
            }
        }
        else {
            if (seaf_db_trans_foreach_selected_row (trans, sql,
                                                    get_gc_id, &gc_id,
                                                    1, "string", origin_repo_id) < 0) {
                seaf_db_rollback (trans);
                seaf_db_trans_close (trans);
                return -1;
            }
        }

        if (g_strcmp0 (last_gc_id, gc_id) != 0) {
            seaf_warning ("Head branch update for repo %s conflicts with GC.\n",
                          branch->repo_id);
            seaf_db_rollback (trans);
            seaf_db_trans_close (trans);
            *gc_conflict = TRUE;
            g_free (gc_id);
            return -1;
        }
        g_free (gc_id);
    }

    switch (seaf_db_type (mgr->seaf->db)) {
    case SEAF_DB_TYPE_MYSQL:
    case SEAF_DB_TYPE_PGSQL:
        sql = "SELECT commit_id FROM Branch WHERE name=? "
            "AND repo_id=? FOR UPDATE";
        break;
    case SEAF_DB_TYPE_SQLITE:
        sql = "SELECT commit_id FROM Branch WHERE name=? "
            "AND repo_id=?";
        break;
    default:
        g_return_val_if_reached (-1);
    }
    if (seaf_db_trans_foreach_selected_row (trans, sql,
                                            get_commit_id, commit_id,
                                            2, "string", branch->name,
                                            "string", branch->repo_id) < 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -1;
    }
    if (strcmp (old_commit_id, commit_id) != 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -1;
    }

    if (scopes_json && scopes_json[0] == '{') {
        int qualification = cloudfile_library_qualification (mgr, trans, branch->repo_id, native_username);
        json_t *snapshot = NULL;
        json_t *conditions = json_loads (scopes_json, JSON_REJECT_DUPLICATES, NULL);
        json_t *context = json_object_get (conditions, "context");
        json_t *target = json_object_get (conditions, "path");
        json_t *lease = json_object_get (conditions, "lease");
        json_t *editing = json_object_get (conditions, "editing");
        json_t *unchanged = json_object_get (conditions, "editing_unchanged");
        const char *path = json_string_value (target);
        gboolean allowed = qualification > 0 && json_is_string (target) &&
            !json_object_get (conditions, "read_transfer_expires_at") &&
            /* A local session only authorizes the download used to prepare a
             * local working copy. Manual publication follows the normal upload
             * path; automatic local commits remain a separate feature. */
            !json_object_get (conditions, "local_session") &&
            !json_object_get (conditions, "local_commit") &&
            (!unchanged || (json_is_true (unchanged) && editing &&
                cloudfile_check_unchanged_file (mgr, branch->repo_id,
                    old_commit_id, path,
                    json_string_value (json_object_get (editing, "base_file_id"))) == 0)) &&
            json_string_length (target) == strlen (path) &&
            cloudfile_check_context (mgr, scopes_json, &snapshot) == 0 &&
            cf_policy_check_write (trans, branch->repo_id, path,
                json_string_value (json_object_get (context, "provider")),
                json_string_value (json_object_get (context, "userId")), snapshot, qualification) == 0 &&
            (!json_is_true (json_object_get (conditions, "create")) ||
                cf_policy_check_create (trans, branch->repo_id, path,
                    json_string_value (json_object_get (context, "provider")),
                    json_string_value (json_object_get (context, "userId")), snapshot, qualification) == 0) &&
            (!lease || seaf_branch_manager_check_read_target (mgr, trans,
                branch->repo_id, path, CF_FILE, old_commit_id,
                json_string_value (json_object_get (lease, "base_version")),
                scopes_json, native_username) == 0) &&
            (!editing || (json_object_get (conditions, "oidc_session") &&
                cloudfile_check_oidc_reference (mgr, trans, conditions) == 0)) &&
            (editing ? (!lease && cf_policy_edit_publish (trans, branch->repo_id,
                path, native_username, conditions, NULL) == 0) :
                cf_policy_check_lease_write (trans, branch->repo_id, path,
                    native_username, lease) == 0);
        if (snapshot) json_decref (snapshot);
        if (conditions) json_decref (conditions);
        if (!allowed) {
            seaf_db_rollback (trans);
            seaf_db_trans_close (trans);
            return -2;
        }
    }

    /* Recheck after policy/qualification waits too. Natural expiry may occur
     * without a refresh taking the SQL scopes held by this transaction. */
    if (scopes_json && cloudfile_check_context (mgr, scopes_json, NULL) < 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -2;
    }

    if (scopes_json && scopes_json[0] == '{') {
        json_t *conditions = json_loads (scopes_json, JSON_REJECT_DUPLICATES, NULL);
        /* SQL row locks prevent refresh/logout/release mutations, but do not
         * stop the database clock. Re-read natural session and lease expiry
         * after all earlier authority/resource waits, immediately before the
         * publication statement. An omitted lease still checks active locks.
         * This also covers compare-only/no-op publication through this path. */
        gboolean current = json_is_object (conditions) &&
            !json_object_get (conditions, "read_transfer_expires_at") &&
            !json_object_get (conditions, "local_session") &&
            !json_object_get (conditions, "local_commit") &&
            cloudfile_check_oidc_reference (mgr, trans, conditions) == 0 &&
            (json_object_get (conditions, "editing") ?
                cf_policy_edit_publish (trans, branch->repo_id,
                    json_string_value (json_object_get (conditions, "path")),
                    native_username, conditions, NULL) == 0 :
                cf_policy_check_lease_write (trans, branch->repo_id,
                    json_string_value (json_object_get (conditions, "path")),
                    native_username,
                    json_object_get (conditions, "lease")) == 0);
        if (conditions) json_decref (conditions);
        if (!current) {
            seaf_db_rollback (trans);
            seaf_db_trans_close (trans);
            return -2;
        }
    }

    if (scopes_json && scopes_json[0] == '{') {
        json_t *conditions = json_loads (scopes_json, JSON_REJECT_DUPLICATES, NULL);
        if (json_object_get (conditions, "editing") &&
            cf_policy_edit_publish (trans, branch->repo_id,
                json_string_value (json_object_get (conditions, "path")),
                native_username, conditions, branch->commit_id) < 0) {
            json_decref (conditions);
            seaf_db_rollback (trans);
            seaf_db_trans_close (trans);
            return -2;
        }
        if (conditions) json_decref (conditions);
    }

    sql = "UPDATE Branch SET commit_id = ? "
        "WHERE name = ? AND repo_id = ?";
    if (seaf_db_trans_query (trans, sql, 3, "string", branch->commit_id,
                             "string", branch->name,
                             "string", branch->repo_id) < 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -1;
    }

    if (seaf_db_commit (trans) < 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -1;
    }

    seaf_db_trans_close (trans);

    /* A successful compare-only/no-op does not publish a new file fact. */
    if (strcmp (old_commit_id, branch->commit_id) != 0)
        on_branch_updated (mgr, branch);

    return 0;
}

int
seaf_branch_manager_test_and_update_branch (SeafBranchManager *mgr, SeafBranch *branch,
    const char *old_commit_id, gboolean check_gc, const char *last_gc_id,
    const char *origin_repo_id, gboolean *gc_conflict)
{
    return test_and_update_branch (mgr, branch, old_commit_id, check_gc, last_gc_id,
                                   origin_repo_id, gc_conflict, NULL, NULL);
}

int
seaf_branch_manager_test_and_update_branch_with_barriers (SeafBranchManager *mgr,
    SeafBranch *branch, const char *old_commit_id, gboolean check_gc,
    const char *last_gc_id, const char *origin_repo_id, gboolean *gc_conflict,
    const char *scopes_json, const char *native_username)
{
    if (!scopes_json || seaf_db_type (mgr->seaf->db) != SEAF_DB_TYPE_MYSQL)
        return -2;
    return test_and_update_branch (mgr, branch, old_commit_id, check_gc, last_gc_id,
                                   origin_repo_id, gc_conflict, scopes_json, native_username);
}

#endif

#ifndef SEAFILE_SERVER
static SeafBranch *
real_get_branch (SeafBranchManager *mgr,
                 const char *repo_id,
                 const char *name)
{
    SeafBranch *branch = NULL;
    sqlite3_stmt *stmt;
    sqlite3 *db;
    char *sql;
    int result;

    pthread_mutex_lock (&mgr->priv->db_lock);

    db = mgr->priv->db;
    sql = sqlite3_mprintf ("SELECT commit_id FROM Branch "
                           "WHERE name = %Q and repo_id='%s'",
                           name, repo_id);
    if (!(stmt = sqlite_query_prepare (db, sql))) {
        seaf_warning ("[Branch mgr] Couldn't prepare query %s\n", sql);
        sqlite3_free (sql);
        pthread_mutex_unlock (&mgr->priv->db_lock);
        return NULL;
    }
    sqlite3_free (sql);

    result = sqlite3_step (stmt);
    if (result == SQLITE_ROW) {
        char *commit_id = (char *)sqlite3_column_text (stmt, 0);

        branch = seaf_branch_new (name, repo_id, commit_id);
        pthread_mutex_unlock (&mgr->priv->db_lock);
        sqlite3_finalize (stmt);
        return branch;
    } else if (result == SQLITE_ERROR) {
        const char *str = sqlite3_errmsg (db);
        seaf_warning ("Couldn't prepare query, error: %d->'%s'\n",
                   result, str ? str : "no error given");
    }

    sqlite3_finalize (stmt);
    pthread_mutex_unlock (&mgr->priv->db_lock);
    return NULL;
}

SeafBranch *
seaf_branch_manager_get_branch (SeafBranchManager *mgr,
                                const char *repo_id,
                                const char *name)
{
    SeafBranch *branch;

    /* "fetch_head" maps to "local" or "master" on client (LAN sync) */
    if (strcmp (name, "fetch_head") == 0) {
        branch = real_get_branch (mgr, repo_id, "local");
        if (!branch) {
            branch = real_get_branch (mgr, repo_id, "master");
        }
        return branch;
    } else {
        return real_get_branch (mgr, repo_id, name);
    }
}

#else

static gboolean
get_branch (SeafDBRow *row, void *vid)
{
    char *ret = vid;
    const char *commit_id;

    commit_id = seaf_db_row_get_column_text (row, 0);
    memcpy (ret, commit_id, 41);

    return FALSE;
}

static SeafBranch *
real_get_branch (SeafBranchManager *mgr,
                 const char *repo_id,
                 const char *name)
{
    char commit_id[41];
    char *sql;

    commit_id[0] = 0;
    sql = "SELECT commit_id FROM Branch WHERE name=? AND repo_id=?";
    if (seaf_db_statement_foreach_row (mgr->seaf->db, sql, 
                                       get_branch, commit_id,
                                       2, "string", name, "string", repo_id) < 0) {
        seaf_warning ("[branch mgr] DB error when get branch %s.\n", name);
        return NULL;
    }

    if (commit_id[0] == 0)
        return NULL;

    return seaf_branch_new (name, repo_id, commit_id);
}

SeafBranch *
seaf_branch_manager_get_branch (SeafBranchManager *mgr,
                                const char *repo_id,
                                const char *name)
{
    SeafBranch *branch;

    /* "fetch_head" maps to "master" on server. */
    if (strcmp (name, "fetch_head") == 0) {
        branch = real_get_branch (mgr, repo_id, "master");
        return branch;
    } else {
        return real_get_branch (mgr, repo_id, name);
    }
}

#endif  /* not SEAFILE_SERVER */

gboolean
seaf_branch_manager_branch_exists (SeafBranchManager *mgr,
                                   const char *repo_id,
                                   const char *name)
{
#ifndef SEAFILE_SERVER
    char *sql;
    gboolean ret;

    pthread_mutex_lock (&mgr->priv->db_lock);

    sql = sqlite3_mprintf ("SELECT name FROM Branch WHERE name = %Q "
                           "AND repo_id='%s'", name, repo_id);
    ret = sqlite_check_for_existence (mgr->priv->db, sql);
    sqlite3_free (sql);

    pthread_mutex_unlock (&mgr->priv->db_lock);
    return ret;
#else
    gboolean db_err = FALSE;

    return seaf_db_statement_exists (mgr->seaf->db,
                                     "SELECT name FROM Branch WHERE name=? "
                                     "AND repo_id=?", &db_err,
                                     2, "string", name, "string", repo_id);
#endif
}

#ifndef SEAFILE_SERVER
GList *
seaf_branch_manager_get_branch_list (SeafBranchManager *mgr,
                                     const char *repo_id)
{
    sqlite3 *db = mgr->priv->db;
    
    int result;
    sqlite3_stmt *stmt;
    char sql[256];
    char *name;
    char *commit_id;
    GList *ret = NULL;
    SeafBranch *branch;

    snprintf (sql, 256, "SELECT name, commit_id FROM branch WHERE repo_id ='%s'",
              repo_id);

    pthread_mutex_lock (&mgr->priv->db_lock);

    if ( !(stmt = sqlite_query_prepare(db, sql)) ) {
        pthread_mutex_unlock (&mgr->priv->db_lock);
        return NULL;
    }

    while (1) {
        result = sqlite3_step (stmt);
        if (result == SQLITE_ROW) {
            name = (char *)sqlite3_column_text(stmt, 0);
            commit_id = (char *)sqlite3_column_text(stmt, 1);
            branch = seaf_branch_new (name, repo_id, commit_id);
            ret = g_list_prepend (ret, branch);
        }
        if (result == SQLITE_DONE)
            break;
        if (result == SQLITE_ERROR) {
            const gchar *str = sqlite3_errmsg (db);
            seaf_warning ("Couldn't prepare query, error: %d->'%s'\n", 
                       result, str ? str : "no error given");
            sqlite3_finalize (stmt);
            seaf_branch_list_free (ret);
            pthread_mutex_unlock (&mgr->priv->db_lock);
            return NULL;
        }
    }

    sqlite3_finalize (stmt);
    pthread_mutex_unlock (&mgr->priv->db_lock);
    return g_list_reverse(ret);
}
#else
static gboolean
get_branches (SeafDBRow *row, void *vplist)
{
    GList **plist = vplist;
    const char *commit_id;
    const char *name;
    const char *repo_id;
    SeafBranch *branch;

    name = seaf_db_row_get_column_text (row, 0);
    repo_id = seaf_db_row_get_column_text (row, 1);
    commit_id = seaf_db_row_get_column_text (row, 2);

    branch = seaf_branch_new (name, repo_id, commit_id);
    *plist = g_list_prepend (*plist, branch);

    return TRUE;
}

GList *
seaf_branch_manager_get_branch_list (SeafBranchManager *mgr,
                                     const char *repo_id)
{
    GList *ret = NULL;
    char *sql;

    sql = "SELECT name, repo_id, commit_id FROM Branch WHERE repo_id=?";
    if (seaf_db_statement_foreach_row (mgr->seaf->db, sql, 
                                       get_branches, &ret,
                                       1, "string", repo_id) < 0) {
        seaf_warning ("[branch mgr] DB error when get branch list.\n");
        return NULL;
    }

    return ret;
}
#endif
