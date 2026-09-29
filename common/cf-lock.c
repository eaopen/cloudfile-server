/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* CloudFile Server Barrier for the unified cf_edit_guard. Default off. */
#include <string.h>

#include "common.h"
#include "cf-ext.h"
#include "cf-fileop.h"
#include "cf-lock.h"
#include "log.h"
#include "seaf-db.h"
#include "seafile-error.h"

static gboolean lock_on = FALSE;
typedef struct {
    char *repo;
    char *path;
} ControlledTarget;

static void
controlled_target_free (gpointer data)
{
    ControlledTarget *target = data;
    if (!target) return;
    g_free (target->repo);
    g_free (target->path);
    g_free (target);
}

static GPrivate controlled_target = G_PRIVATE_INIT(controlled_target_free);

void
cf_lock_controlled_publish_enter (const char *repo, const char *path)
{
    ControlledTarget *target = g_new0 (ControlledTarget, 1);
    target->repo = g_strdup (repo);
    target->path = g_strdup (path);
    g_private_replace (&controlled_target, target);
}

void
cf_lock_controlled_publish_leave (void)
{
    g_private_replace (&controlled_target, NULL);
}

static gboolean
barrier_row (SeafDBRow *db_row, void *data)
{
    (void)db_row;
    (void)data;
    return FALSE;
}

static char *
like_descendant_pattern (const char *path)
{
    if (strcmp (path, "/") == 0)
        return g_strdup ("/%");
    GString *pattern = g_string_new ("");
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '%' || *p == '_')
            g_string_append_c (pattern, '\\');
        g_string_append_c (pattern, *p);
    }
    g_string_append (pattern, "/%");
    return g_string_free (pattern, FALSE);
}

/* Lock and resource share the Hub migration's UID and seafile-db. The SQL
 * retains manual locks and suspended checkout reservations.
 * A failed query refuses the write rather than weakening the barrier. */
static int
load_exact (const char *repo_id, const char *path, const char *user)
{
    char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    int n = 0;
    if (n == 0)
        n = seaf_db_statement_foreach_row (
            cf_ext_database (),
            "SELECT r.uid FROM cf_resource r JOIN cf_edit_guard g ON g.resource_uid=r.uid "
            "WHERE r.repo_id=? AND r.path_hash=? AND r.path=? AND r.kind='file' AND r.state='active' "
            "AND g.repo_id=r.repo_id AND g.guard_id IS NOT NULL "
            "AND (g.lifecycle_ref<>r.lifecycle_ref OR g.mode IS NULL OR g.mode<>'file-lock' OR g.owner_native_user IS NULL OR g.owner_native_user<>?) LIMIT 1",
            barrier_row, NULL, 4, "string", repo_id, "string", hash,
            "string", path, "string", user ? user : "");

    g_free (hash);
    return n;
}

static int
load_descendant (const char *repo_id, const char *path)
{
    char *pattern = like_descendant_pattern (path);
    int n = 0;
    if (n == 0)
        n = seaf_db_statement_foreach_row (
            cf_ext_database (),
            "SELECT r.uid FROM cf_resource r JOIN cf_edit_guard g ON g.resource_uid=r.uid "
            "WHERE r.repo_id=? AND r.path LIKE ? ESCAPE '\\\\' AND r.state='active' "
            "AND g.repo_id=r.repo_id AND g.guard_id IS NOT NULL LIMIT 1",
            barrier_row, NULL, 2, "string", repo_id, "string", pattern);

    g_free (pattern);
    return n;
}

static int
check_path (const char *repo_id, const char *path, const char *user,
            GError **error)
{
    int n;
    if (!cf_ext_database ())
        goto unavailable;
    int ready = seaf_db_statement_foreach_row (
        cf_ext_database (),
        "SELECT version FROM cf_schema_migration WHERE version='029_editing_core' "
        "AND state='applied' AND step=2", barrier_row, NULL, 0);
    if (ready != 1)
        goto unavailable;
    n = load_exact (repo_id, path, user);
    if (n < 0)
        goto unavailable;
    if (n > 0)
        goto locked;
    n = load_descendant (repo_id, path);
    if (n < 0)
        goto unavailable;
    if (n > 0)
        goto locked; /* Parent operations are conservative, even for owner. */
    return 0;

unavailable:
    g_set_error (error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL,
                 "CloudFile lock barrier unavailable");
    return -1;
locked:
    g_set_error (error, SEAFILE_DOMAIN, CF_ERR_FILE_LOCKED,
                 "File %s is locked", path);
    return -1;
}

static int
cf_lock_prepare (const CfFileOp *fop, GError **error)
{
    if (!lock_on || cf_fileop_op_pathless (fop->op) || !fop->repo_id)
        return 0;
    GList *subjects = cf_fileop_subject_paths (fop);
    GList *sources = cf_fileop_source_paths (fop);
    ControlledTarget *controlled = g_private_get (&controlled_target);
    if (controlled && g_strcmp0 (fop->op, CF_OP_UPDATE_FILE) == 0 &&
        g_strcmp0 (fop->repo_id, controlled->repo) == 0 &&
        subjects && !subjects->next && !sources &&
        g_strcmp0 (subjects->data, controlled->path) == 0) {
        g_list_free_full (subjects, g_free);
        return 0;
    }
    int ret = 0;
    for (GList *p = subjects; p && ret == 0; p = p->next)
        ret = check_path (fop->repo_id, p->data, fop->user, error);
    const char *source_repo = fop->src_repo_id ? fop->src_repo_id : fop->repo_id;
    for (GList *p = sources; p && ret == 0; p = p->next)
        ret = check_path (source_repo, p->data, fop->user, error);
    g_list_free_full (subjects, g_free);
    g_list_free_full (sources, g_free);
    return ret;
}

gboolean
cf_lock_enabled (void)
{
    return lock_on;
}

void
cf_lock_init (void)
{
    char *backend = cf_ext_config_string ("lock_backend");
    lock_on = cf_ext_config_bool ("file_lock_enabled") &&
              (!backend || strcmp (backend, "cloudfile") == 0);
    if (backend && strcmp (backend, "cloudfile") != 0)
        seaf_warning ("CloudFile: unsupported lock backend '%s'.\n", backend);
    g_free (backend);
    if (lock_on) {
        cf_fileop_register ("file-lock", cf_lock_prepare, NULL, NULL);
        seaf_message ("CloudFile: UID lease write barrier enabled.\n");
    }
}
