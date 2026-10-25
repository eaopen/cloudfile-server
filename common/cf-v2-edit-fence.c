/* CF-EDIT V2: deny unscoped native publications touching reserved paths.
 *
 * The candidate diff is prepared before Branch CAS. Only current Guard rows
 * are locked and checked after Branch(master) in the final short transaction.
 * Checkout writers use the same Branch -> Guard lock order.
 * This is a fail-closed fence, NOT an authorization path for Commit/Checkin.
 */
#include "cf-v2-edit-fence.h"
#include "common.h"
#include "commit-mgr.h"
#include "diff-simple.h"
#include "log.h"
#include "repo-mgr.h"
#include "seafile-session.h"

typedef struct {
    GPtrArray *paths;
    gboolean invalid;
} GuardPaths;

static gboolean
read_guard_path(SeafDBRow *row, void *data)
{
    GuardPaths *state = data;
    const char *path = seaf_db_row_get_column_text(row, 0);
    if (!path || path[0] != '/' || strlen(path) > 4096 || !g_utf8_validate(path, -1, NULL)) {
        state->invalid = TRUE;
        return FALSE;
    }
    if (state->paths->len >= 8192) {
        state->invalid = TRUE;
        return FALSE;
    }
    g_ptr_array_add(state->paths, g_strdup(path));
    return TRUE;
}

static gboolean
path_touches(const char *changed, const char *guarded)
{
    if (!changed || !*changed)
        return FALSE;
    const char *name = changed[0] == '/' ? changed + 1 : changed;
    char *absolute = g_strconcat("/", name, NULL);
    gboolean touches = !strcmp(absolute, guarded) ||
        (g_str_has_prefix(guarded, absolute) && guarded[strlen(absolute)] == '/');
    g_free(absolute);
    return touches;
}

/* Two policies only: OFF (default) and OPTIONAL (listed repository). Parse the
 * startup configuration once; never query SQL for an OFF repository. */
static gsize checkout_policy_state = 0;  /* 0=unread, 1=valid, 2=invalid */
static GHashTable *checkout_repo_ids;

static gboolean
canonical_repo_id(const char *id)
{
    if (!id || strlen(id) != 36)
        return FALSE;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id[i] != '-')
                return FALSE;
        } else if (!g_ascii_isdigit(id[i]) && !(id[i] >= 'a' && id[i] <= 'f')) {
            return FALSE;
        }
    }
    return TRUE;
}

int
cf_v2_edit_repo_checkout(struct _SeafileSession *session, const char *repo_id)
{
    if (g_once_init_enter(&checkout_policy_state)) {
        gboolean valid = TRUE;
        GError *error = NULL;
        gboolean enabled = session && session->config &&
            g_key_file_get_boolean(session->config, "cloudfile",
                                   "checkout_enabled", &error);
        if (error) {
            /* Missing setting means OFF; an explicitly invalid setting fails closed. */
            valid = error->domain == G_KEY_FILE_ERROR &&
                (error->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND ||
                 error->code == G_KEY_FILE_ERROR_GROUP_NOT_FOUND);
            g_clear_error(&error);
        }
        checkout_repo_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        if (valid && enabled) {
            char *raw = g_key_file_get_string(session->config, "cloudfile",
                                              "checkout_repo_ids", NULL);
            if (raw && strlen(raw) > 8192)
                valid = FALSE;
            if (raw && *g_strstrip(raw)) {
                char **parts = g_strsplit(raw, ",", -1);
                for (char **p = parts; *p && valid; ++p) {
                    char *id = g_strstrip(*p);
                    if (!canonical_repo_id(id) ||
                        g_hash_table_contains(checkout_repo_ids, id))
                        valid = FALSE;
                    else
                        g_hash_table_add(checkout_repo_ids, g_strdup(id));
                }
                g_strfreev(parts);
            }
            g_free(raw);
        }
        if (!valid)
            seaf_warning("CloudFile: invalid per-repo Checkout configuration; writes denied.\n");
        g_once_init_leave(&checkout_policy_state, valid ? 1 : 2);
    }
    if (checkout_policy_state == 2)
        return -1;
    if (!repo_id)
        return 0;
    return g_hash_table_contains(checkout_repo_ids, repo_id) ? 1 : 0;
}

/* Schema is validated on guarded paths only. Its absence is a failure for an
 * OPTIONAL repository, never a reason to bypass an active Checkout. */
static gsize guard_schema_state = 0; /* 0=unread, 1=absent, 2=installed, 3=error */

static gboolean
guard_table_found(SeafDBRow *row, void *data)
{
    (void)row;
    *(gboolean *)data = TRUE;
    return FALSE;
}

static int
guard_available(struct _SeafileSession *session)
{
    if (!session || !session->db ||
        seaf_db_type(session->db) != SEAF_DB_TYPE_MYSQL)
        return -1;
    if (g_once_init_enter(&guard_schema_state)) {
        gboolean found = FALSE;
        int count = seaf_db_statement_foreach_row(session->db,
            "SELECT 1 FROM information_schema.TABLES "
            "WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_guard' LIMIT 1",
            guard_table_found, &found, 0);
        g_once_init_leave(&guard_schema_state, count < 0 ? 3 : found ? 2 : 1);
    }
    return guard_schema_state == 2 ? 1 : -1;
}

/* The Guard policy is OFF by default. For OPTIONAL libraries, resolve the
 * candidate commit's changed paths before opening the final Branch transaction.
 * DiffEntry objects are immutable for this candidate: a racing HEAD is caught
 * by the final Branch CAS, and new Guard reservations are read under Branch. */
struct _CfV2EditPrepared {
    char *repo_id;
    char *old_id;
    char *new_id;
    GList *changes;
};

void
cf_v2_edit_prepared_free(CfV2EditPrepared *prepared)
{
    if (!prepared)
        return;
    g_list_free_full(prepared->changes, (GDestroyNotify)diff_entry_free);
    g_free(prepared->repo_id);
    g_free(prepared->old_id);
    g_free(prepared->new_id);
    g_free(prepared);
}

CfV2EditPrepared *
cf_v2_edit_prepare_unscoped_branch(struct _SeafileSession *session,
                                   const char *repo_id,
                                   const char *old_commit_id,
                                   const char *new_commit_id)
{
    if (!session || !repo_id || !old_commit_id || !new_commit_id)
        return NULL;
    int policy = cf_v2_edit_repo_checkout(session, repo_id);
    if (policy < 0)
        return NULL;
    CfV2EditPrepared *prepared = g_new0(CfV2EditPrepared, 1);
    prepared->repo_id = g_strdup(repo_id);
    prepared->old_id = g_strdup(old_commit_id);
    prepared->new_id = g_strdup(new_commit_id);
    if (policy == 0 || !strcmp(old_commit_id, new_commit_id))
        return prepared;
    if (guard_available(session) <= 0)
        goto fail;
    SeafRepo *repo = seaf_repo_manager_get_repo(session->repo_mgr, repo_id);
    if (!repo)
        goto fail;
    SeafCommit *old_commit = seaf_commit_manager_get_commit(
        session->commit_mgr, repo_id, repo->version, old_commit_id);
    SeafCommit *new_commit = seaf_commit_manager_get_commit(
        session->commit_mgr, repo_id, repo->version, new_commit_id);
    int result = (old_commit && new_commit)
        ? diff_commit_roots(repo->store_id, repo->version,
                            old_commit->root_id, new_commit->root_id,
                            &prepared->changes, FALSE)
        : -1;
    if (old_commit)
        seaf_commit_unref(old_commit);
    if (new_commit)
        seaf_commit_unref(new_commit);
    seaf_repo_unref(repo);
    if (result < 0)
        goto fail;
    return prepared;
fail:
    cf_v2_edit_prepared_free(prepared);
    return NULL;
}

int
cf_v2_edit_check_unscoped_branch(SeafDBTrans *trans,
                                 struct _SeafileSession *session,
                                 const char *repo_id,
                                 const char *old_commit_id,
                                 const char *new_commit_id,
                                 const CfV2EditPrepared *prepared)
{
    if (!trans || !session || !repo_id || !old_commit_id || !new_commit_id ||
        !prepared || g_strcmp0(prepared->repo_id, repo_id) ||
        g_strcmp0(prepared->old_id, old_commit_id) ||
        g_strcmp0(prepared->new_id, new_commit_id))
        return -1;
    int policy = cf_v2_edit_repo_checkout(session, repo_id);
    if (policy <= 0)
        return policy;
    if (guard_available(session) <= 0)
        return -1;
    if (seaf_db_type(session->db) != SEAF_DB_TYPE_MYSQL)
        return -1;

    GuardPaths guards = {g_ptr_array_new_with_free_func(g_free), FALSE};
    int count = seaf_db_trans_foreach_selected_row(trans,
        "SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) "
        "WHERE repo_id=? AND guard_id IS NOT NULL "
        "AND lease_expires_at>UTC_TIMESTAMP(6) FOR UPDATE",
        read_guard_path, &guards, 1, "string", repo_id);
    int rc = -1;
    if (count < 0 || guards.invalid)
        goto done;
    rc = 0;
    for (GList *entry = prepared->changes; entry != NULL && rc == 0;
         entry = entry->next) {
        DiffEntry *delta = entry->data;
        for (guint i = 0; i < guards.paths->len; ++i) {
            const char *path = g_ptr_array_index(guards.paths, i);
            if (path_touches(delta->name, path) ||
                path_touches(delta->new_name, path)) {
                rc = -1;
                break;
            }
        }
    }
done:
    g_ptr_array_free(guards.paths, TRUE);
    return rc;
}
