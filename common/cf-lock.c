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

/* V2 reservations are path-keyed. Unscoped writes, including the owner's,
 * must not consume an exclusive Checkout without a verified publish proof. */
static int
load_exact (const char *repo_id, const char *path, const char *user)
{
    (void)user;
    char *hash = g_compute_checksum_for_string (G_CHECKSUM_SHA256, path, -1);
    int n = seaf_db_statement_foreach_row (
        cf_ext_database (),
        "SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) "
        "WHERE repo_id=? AND path_hash=UNHEX(?) AND path=? "
        "AND guard_id IS NOT NULL AND lease_expires_at>UTC_TIMESTAMP(6) LIMIT 1",
        barrier_row, NULL, 3, "string", repo_id, "string", hash, "string", path);

    g_free (hash);
    return n;
}

static int
load_descendant (const char *repo_id, const char *path)
{
    char *pattern = like_descendant_pattern (path);
    int n = seaf_db_statement_foreach_row (
        cf_ext_database (),
        "SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) "
        "WHERE repo_id=? AND path LIKE ? ESCAPE '\\\\' "
        "AND guard_id IS NOT NULL AND lease_expires_at>UTC_TIMESTAMP(6) LIMIT 1",
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
    /* Checkout is unfinished and opt-in. A legacy file-lock switch alone
     * must not make normal uploads depend on Guard or the retired ledger. */
    lock_on = cf_ext_config_bool ("checkout_enabled") &&
              (!backend || strcmp (backend, "cloudfile") == 0);
    if (backend && strcmp (backend, "cloudfile") != 0)
        seaf_warning ("CloudFile: unsupported lock backend '%s'.\n", backend);
    g_free (backend);
    if (lock_on) {
        cf_fileop_register ("file-lock", cf_lock_prepare, NULL, NULL);
        seaf_message ("CloudFile: V2 Checkout write barrier enabled.\n");
    }
}
