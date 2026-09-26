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
#endif

#include "branch-mgr.h"
#ifdef SEAFILE_SERVER
#include <jansson.h>
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
cloudfile_check_barriers (SeafDBTrans *trans, const char *repo_id, const char *scopes_json)
{
    json_t *scopes = NULL;
    GPtrArray *ordered = g_ptr_array_new_with_free_func (cloudfile_scope_free);
    char *database = NULL;
    int result = -1;
    gboolean has_user = FALSE, has_repo = FALSE;
    if (strlen (scopes_json) > 16384)
        goto out;
    scopes = json_loads (scopes_json, JSON_REJECT_DUPLICATES, NULL);
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
        int locked = seaf_db_trans_acquire_scope_lock (trans, name, 5);
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
    result = 0;
out:
    g_free (database);
    g_ptr_array_free (ordered, TRUE);
    if (scopes)
        json_decref (scopes);
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

static int
cloudfile_check_repo (SeafDBTrans *trans, const char *repo_id)
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
            cloudfile_repo_status, &writable, 1, "string", repo_id) != 1 || !writable)
        return -1;
    if (seaf_db_trans_check_for_existence (trans,
            "SELECT repo_id FROM VirtualRepo WHERE repo_id=? FOR UPDATE", &error,
            1, "string", repo_id) || error)
        return -1;
    return 0;
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

    if (scopes_json && (cloudfile_check_barriers (trans, branch->repo_id, scopes_json) < 0 ||
                       ccnet_user_manager_lock_active_account (mgr->seaf->user_mgr, trans, native_username) < 0 ||
                       cloudfile_check_repo (trans, branch->repo_id) < 0)) {
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
