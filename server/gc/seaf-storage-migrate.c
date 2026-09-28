/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include "common.h"
#include "log.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>

#include "seafile-session.h"
#include "seaf-utils.h"
#include "utils.h"

static char *ccnet_dir = NULL;
static char *seafile_dir = NULL;
static char *central_config_dir = NULL;

SeafileSession *seaf;

typedef struct {
    const char *kind;
    guint64 count;
} CopyProgress;

static const char *short_opts = "hc:d:F:";
static const struct option long_opts[] = {
    { "help", no_argument, NULL, 'h' },
    { "config-file", required_argument, NULL, 'c' },
    { "seafdir", required_argument, NULL, 'd' },
    { "central-config-dir", required_argument, NULL, 'F' },
    { 0, 0, 0, 0 },
};

static void
usage (void)
{
    fprintf (stderr,
             "usage: seaf-storage-migrate [-c config_dir] "
             "[-d seafile_dir] [-F central_config_dir] "
             "<repo_id> <target_storage_id>\n"
             "The seaf-server and fileserver processes must be stopped. "
             "Source objects are retained for rollback.\n");
}

static gboolean
backend_is_multiple (GKeyFile *config, const char *section)
{
    char *name = g_key_file_get_string (config, section, "name", NULL);
    gboolean ret = g_strcmp0 (name, "multiple") == 0;

    g_free (name);
    return ret;
}

static gboolean
pid_is_running (const char *pid_path)
{
    char *contents = NULL;
    char *end = NULL;
    gint64 value;
    gboolean running = FALSE;

    if (!g_file_get_contents (pid_path, &contents, NULL, NULL))
        return FALSE;
    errno = 0;
    value = g_ascii_strtoll (contents, &end, 10);
    if (errno == 0 && end != contents && value > 1 && value <= G_MAXINT &&
        (kill ((pid_t)value, 0) == 0 || errno == EPERM))
        running = TRUE;
    g_free (contents);
    return running;
}

static gboolean
storage_processes_are_stopped (const char *data_dir)
{
    char *top_dir = g_path_get_dirname (data_dir);
    char *server_pid = g_build_filename (top_dir, "pids",
                                         "seaf-server.pid", NULL);
    char *fileserver_pid = g_build_filename (top_dir, "pids",
                                             "fileserver.pid", NULL);
    gboolean stopped = !pid_is_running (server_pid) &&
        !pid_is_running (fileserver_pid);

    if (!stopped)
        seaf_warning ("Storage migration requires seaf-server and "
                      "fileserver to be stopped.\n");
    g_free (server_pid);
    g_free (fileserver_pid);
    g_free (top_dir);
    return stopped;
}

static void
copy_progress (const char *store_id, guint64 count, void *user_data)
{
    CopyProgress *progress = user_data;

    progress->count = count;
    if (count % 1000 == 0)
        seaf_message ("Copied %"G_GUINT64_FORMAT" %s objects for %.8s.\n",
                      count, progress->kind, store_id);
}

static int
set_repo_storage_id (const char *repo_id, const char *storage_id)
{
    SeafDBTrans *trans = seaf_db_begin_transaction (seaf->db);

    if (!trans)
        return -1;
    if (seaf_db_trans_query (
            trans, "DELETE FROM RepoStorageId WHERE repo_id = ?", 1,
            "string", repo_id) < 0 ||
        seaf_db_trans_query (
            trans, "INSERT INTO RepoStorageId (repo_id, storage_id) "
            "VALUES (?, ?)", 2, "string", repo_id,
            "string", storage_id) < 0 ||
        seaf_db_commit (trans) < 0) {
        seaf_db_rollback (trans);
        seaf_db_trans_close (trans);
        return -1;
    }
    seaf_db_trans_close (trans);
    return 0;
}

static int
copy_commit_store (const char *repo_id, int version,
                   const char *source, const char *target,
                   guint64 *count)
{
    CopyProgress progress = { "commit", 0 };
    int ret = seaf_obj_store_copy_store (
        seaf->commit_mgr->obj_store, repo_id, version, source, target,
        copy_progress, &progress);

    *count += progress.count;
    return ret;
}

static int
copy_virtual_repo_commits (SeafRepo *repo,
                           const char *source, const char *target,
                           guint64 *count)
{
    GList *ids = seaf_repo_manager_get_virtual_repo_ids_by_origin (
        seaf->repo_mgr, repo->id);
    GList *ptr;
    int ret = 0;

    for (ptr = ids; ptr; ptr = ptr->next) {
        SeafRepo *virtual_repo = seaf_repo_manager_get_repo (
            seaf->repo_mgr, ptr->data);
        if (!virtual_repo) {
            seaf_warning ("Failed to load virtual repo %s.\n",
                          (char *)ptr->data);
            ret = -1;
            break;
        }
        ret = copy_commit_store (virtual_repo->id, virtual_repo->version,
                                 source, target, count);
        seaf_repo_unref (virtual_repo);
        if (ret < 0)
            break;
    }
    string_list_free (ids);
    return ret;
}

static int
verify_route (SeafRepo *repo, const char *storage_id)
{
    char *commit_id = seaf_obj_store_get_storage_id (
        seaf->commit_mgr->obj_store, repo->id);
    char *fs_id = seaf_obj_store_get_storage_id (
        seaf->fs_mgr->obj_store, repo->store_id);
    char *block_id = seaf_block_manager_get_storage_id (
        seaf->block_mgr, repo->store_id);
    int ret = g_strcmp0 (commit_id, storage_id) == 0 &&
        g_strcmp0 (fs_id, storage_id) == 0 &&
        g_strcmp0 (block_id, storage_id) == 0 ? 0 : -1;

    g_free (commit_id);
    g_free (fs_id);
    g_free (block_id);
    return ret;
}

static int
migrate_repo (const char *repo_id, const char *target)
{
    SeafRepo *repo = NULL;
    char *source = NULL;
    char *fs_source = NULL;
    char *block_source = NULL;
    guint64 commit_count = 0;
    CopyProgress fs_progress = { "fs", 0 };
    CopyProgress block_progress = { "block", 0 };
    int ret = -1;

    repo = seaf_repo_manager_get_repo (seaf->repo_mgr, repo_id);
    if (!repo) {
        seaf_warning ("Repo %s does not exist or has a damaged head.\n",
                      repo_id);
        goto out;
    }
    if (repo->is_virtual) {
        seaf_warning ("Migrate the origin repo %s instead of virtual repo %s.\n",
                      repo->store_id, repo->id);
        goto out;
    }

    source = seaf_obj_store_get_storage_id (
        seaf->commit_mgr->obj_store, repo->id);
    fs_source = seaf_obj_store_get_storage_id (
        seaf->fs_mgr->obj_store, repo->store_id);
    block_source = seaf_block_manager_get_storage_id (
        seaf->block_mgr, repo->store_id);
    if (!source || g_strcmp0 (source, fs_source) != 0 ||
        g_strcmp0 (source, block_source) != 0) {
        seaf_warning ("Commit, fs and block storage routes disagree for "
                      "repo %s.\n", repo->id);
        goto out;
    }
    if (g_strcmp0 (source, target) == 0) {
        seaf_message ("Repo %s already uses storage class %s.\n",
                      repo->id, target);
        ret = 0;
        goto out;
    }
    if (!seaf_obj_store_has_storage_id (
            seaf->commit_mgr->obj_store, target) ||
        !seaf_obj_store_has_storage_id (
            seaf->fs_mgr->obj_store, target) ||
        !seaf_block_manager_has_storage_id (
            seaf->block_mgr, target)) {
        seaf_warning ("Target storage class %s is not configured for "
                      "commit, fs and block backends.\n", target);
        goto out;
    }

    seaf_message ("Copying repo %s from %s to %s.\n",
                  repo->id, source, target);
    if (copy_commit_store (repo->id, repo->version, source, target,
                           &commit_count) < 0 ||
        copy_virtual_repo_commits (repo, source, target,
                                   &commit_count) < 0 ||
        seaf_obj_store_copy_store (
            seaf->fs_mgr->obj_store, repo->store_id, repo->version,
            source, target, copy_progress, &fs_progress) < 0 ||
        seaf_block_manager_copy_store (
            seaf->block_mgr, repo->store_id, repo->version,
            source, target, copy_progress, &block_progress) < 0) {
        seaf_warning ("Copy or target verification failed; storage route "
                      "was not changed.\n");
        goto out;
    }

    if (set_repo_storage_id (repo->id, target) < 0 ||
        verify_route (repo, target) < 0) {
        seaf_warning ("Failed to switch route to %s; restoring %s.\n",
                      target, source);
        set_repo_storage_id (repo->id, source);
        goto out;
    }

    seaf_message ("Storage migration finished: %"G_GUINT64_FORMAT
                  " commits, %"G_GUINT64_FORMAT" fs objects and %"
                  G_GUINT64_FORMAT" blocks copied and verified. "
                  "Source objects were retained.\n",
                  commit_count, fs_progress.count, block_progress.count);
    ret = 0;

out:
    g_free (source);
    g_free (fs_source);
    g_free (block_source);
    if (repo)
        seaf_repo_unref (repo);
    return ret;
}

int
main (int argc, char **argv)
{
    int c;
    const char *repo_id;
    const char *target;

    ccnet_dir = DEFAULT_CONFIG_DIR;
    while ((c = getopt_long (argc, argv, short_opts,
                             long_opts, NULL)) != EOF) {
        switch (c) {
        case 'h':
            usage ();
            return 0;
        case 'c':
            ccnet_dir = g_strdup (optarg);
            break;
        case 'd':
            seafile_dir = g_strdup (optarg);
            break;
        case 'F':
            central_config_dir = g_strdup (optarg);
            break;
        default:
            usage ();
            return 1;
        }
    }
    if (argc - optind != 2) {
        usage ();
        return 1;
    }
    repo_id = argv[optind];
    target = argv[optind + 1];
    if (!is_uuid_valid (repo_id) || !target[0]) {
        usage ();
        return 1;
    }
    if (seafile_log_init ("-", "info", "debug",
                          "seaf-storage-migrate") < 0)
        return 1;
    if (!seafile_dir)
        seafile_dir = g_build_filename (ccnet_dir, "seafile-data", NULL);
    if (!storage_processes_are_stopped (seafile_dir))
        return 1;

    seaf = seafile_session_new (central_config_dir, seafile_dir,
                                ccnet_dir, TRUE);
    if (!seaf) {
        seaf_warning ("Failed to create seafile session.\n");
        return 1;
    }
    if (!backend_is_multiple (seaf->config, "commit_object_backend") ||
        !backend_is_multiple (seaf->config, "fs_object_backend") ||
        !backend_is_multiple (seaf->config, "block_backend")) {
        seaf_warning ("Storage migration requires multiple backend mode "
                      "for commit, fs and block stores.\n");
        return 1;
    }

    return migrate_repo (repo_id, target) < 0 ? 1 : 0;
}
