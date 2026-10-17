/* CF-EDIT V2: deny unscoped native publications touching reserved paths.
 *
 * All reads happen *within* the native Branch CAS transaction, after locking
 * Branch(master). The Checkout service uses the same Branch -> Guard order.
 * This is a fail-closed fence, NOT an authorization path for Commit/Checkin.
 */
#include "cf-v2-edit-fence.h"
#include "common.h"
#include "commit-mgr.h"
#include "diff-simple.h"
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

/* Schema is the sole Checkout capability source. Cache only for this native
 * process; installing/removing Guard requires a controlled restart. An SQL
 * failure is permanently fail-closed until restart, never an absent Guard. */
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
    return guard_schema_state == 3 ? -1 : guard_schema_state == 2 ? 1 : 0;
}

int
cf_v2_edit_check_unscoped_branch(SeafDBTrans *trans,
                                 struct _SeafileSession *session,
                                 const char *repo_id,
                                 const char *old_commit_id,
                                 const char *new_commit_id)
{
    if (!trans || !session || !repo_id || !old_commit_id || !new_commit_id)
        return -1;
    int active = guard_available(session);
    if (active <= 0)
        return active == 0 ? 0 : -1;
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
    if (guards.paths->len == 0 || !strcmp(old_commit_id, new_commit_id)) {
        rc = 0;
        goto done;
    }

    SeafRepo *repo = seaf_repo_manager_get_repo(session->repo_mgr, repo_id);
    if (!repo)
        goto done;
    SeafCommit *old_commit = seaf_commit_manager_get_commit(
        session->commit_mgr, repo_id, repo->version, old_commit_id);
    SeafCommit *new_commit = seaf_commit_manager_get_commit(
        session->commit_mgr, repo_id, repo->version, new_commit_id);
    GList *changes = NULL;
    if (!old_commit || !new_commit ||
        diff_commit_roots(repo->store_id, repo->version,
                          old_commit->root_id, new_commit->root_id,
                          &changes, FALSE) < 0)
        goto cleanup;

    rc = 0;
    for (GList *entry = changes; entry != NULL && rc == 0; entry = entry->next) {
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

cleanup:
    g_list_free_full(changes, (GDestroyNotify)diff_entry_free);
    if (old_commit)
        seaf_commit_unref(old_commit);
    if (new_commit)
        seaf_commit_unref(new_commit);
    seaf_repo_unref(repo);
done:
    g_ptr_array_free(guards.paths, TRUE);
    return rc;
}
