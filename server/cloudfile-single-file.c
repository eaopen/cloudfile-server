/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "common.h"
#include "cloudfile-single-file.h"
#include <string.h>

typedef struct {
    SeafFSManager *fs;
    const char *store, *target, *username;
    char *old_file, *new_file;
    int changes;
} SingleFile;

typedef struct {
    SeafFSManager *fs;
    const char *store, *old_file, *new_file, *username;
    gint64 size, mtime;
} BuildFile;

static gboolean entry_valid (SeafDirent *entry)
{
    return entry && entry->name && *entry->name &&
        strlen(entry->name) == entry->name_len && entry->name_len < SEAF_DIR_NAME_LEN &&
        g_utf8_validate(entry->name, -1, NULL) && !strchr(entry->name, '/') &&
        strcmp(entry->name, ".") && strcmp(entry->name, "..") &&
        strlen(entry->id) == 40 && strspn(entry->id, "0123456789abcdef") == 40;
}

static gboolean same_metadata (SeafDirent *a, SeafDirent *b)
{
    return a->version == b->version && a->mode == b->mode && a->mtime == b->mtime &&
        a->size == b->size && !g_strcmp0(a->modifier, b->modifier);
}

static int single_tree (SingleFile *check, const char *old_root, const char *new_root,
                       const char *parent)
{
    SeafDir *old = seaf_fs_manager_get_seafdir(check->fs, check->store, 1, old_root);
    SeafDir *updated = seaf_fs_manager_get_seafdir(check->fs, check->store, 1, new_root);
    int result = -1;
    if (!old || !updated || old->version != 1 || updated->version != 1) goto out;
    GList *left = old->entries, *right = updated->entries;
    const char *previous = NULL;
    while (left && right) {
        SeafDirent *a = left->data, *b = right->data;
        if (!entry_valid(a) || !entry_valid(b) || strcmp(a->name, b->name) ||
            (previous && strcmp(previous, a->name) <= 0)) goto out;
        previous = a->name; // Native SeafDir entries are strictly descending.
        char *entry_path = g_strconcat(parent, a->name, NULL);
        gboolean exact = !strcmp(check->target, entry_path);
        gboolean ancestor = g_str_has_prefix(check->target, entry_path) &&
            check->target[strlen(entry_path)] == '/';
        gboolean equal = !strcmp(a->id, b->id) && same_metadata(a, b);
        if (exact) {
            if (!S_ISREG(a->mode) || !S_ISREG(b->mode) || a->mode != b->mode ||
                a->version != 1 || b->version != 1 || b->mtime < 0 || b->size < 0 ||
                g_strcmp0(b->modifier, check->username) || equal || check->changes) {
                g_free(entry_path); goto out;
            }
            Seafile *file = seaf_fs_manager_get_seafile(check->fs, check->store, 1, b->id);
            if (!file || file->file_size > G_MAXINT64 || (gint64)file->file_size != b->size) {
                if (file) seafile_unref(file);
                g_free(entry_path); goto out;
            }
            seafile_unref(file);
            memcpy(check->old_file, a->id, 41);
            memcpy(check->new_file, b->id, 41);
            ++check->changes;
        }
        else if (ancestor) {
            /* Only target ancestors may carry newly computed directory IDs or
             * mtimes. Name/type/mode/version and other metadata remain stable. */
            if (!S_ISDIR(a->mode) || !S_ISDIR(b->mode) || a->mode != b->mode ||
                a->version != b->version || a->size != b->size || g_strcmp0(a->modifier, b->modifier)) {
                g_free(entry_path); goto out;
            }
            char *child = g_strconcat(entry_path, "/", NULL);
            int child_result = single_tree(check, a->id, b->id, child);
            g_free(child);
            if (child_result < 0) { g_free(entry_path); goto out; }
        }
        else if (!equal) { g_free(entry_path); goto out; }
        g_free(entry_path);
        left = left->next;
        right = right->next;
    }
    if (left || right) goto out;
    result = 0;
out:
    if (old) seaf_dir_free(old);
    if (updated) seaf_dir_free(updated);
    return result;
}

int cf_check_single_file_candidate (SeafFSManager *fs, const char *store_id,
                                    SeafCommit *base, SeafCommit *candidate,
                                    const char *path, const char *native_username,
                                    char old_file[41], char new_file[41])
{
    if (old_file) old_file[0] = 0;
    if (new_file) new_file[0] = 0;
    if (!fs || !store_id || !base || !candidate || !old_file || !new_file || !path ||
        !native_username || !*native_username || path[0] != '/' || !path[1] || strlen(path) > 4096 ||
        !g_utf8_validate(path, -1, NULL) || path[strlen(path)-1] == '/' || strstr(path, "//") ||
        strcmp(store_id, base->repo_id) || strcmp(base->repo_id, candidate->repo_id) ||
        base->version != 1 || candidate->version != 1 || base->encrypted || candidate->encrypted ||
        g_strcmp0(candidate->parent_id, base->commit_id) || candidate->second_parent_id ||
        candidate->conflict || candidate->repaired || candidate->new_merge ||
        g_strcmp0(candidate->creator_name, native_username) ||
        g_strcmp0(base->repo_name, candidate->repo_name) || g_strcmp0(base->repo_desc, candidate->repo_desc) ||
        g_strcmp0(base->repo_category, candidate->repo_category) || base->no_local_history != candidate->no_local_history ||
        base->enc_version != candidate->enc_version || g_strcmp0(base->magic, candidate->magic) ||
        g_strcmp0(base->random_key, candidate->random_key) || g_strcmp0(base->salt, candidate->salt) ||
        g_strcmp0(base->pwd_hash, candidate->pwd_hash) || g_strcmp0(base->pwd_hash_algo, candidate->pwd_hash_algo) ||
        g_strcmp0(base->pwd_hash_params, candidate->pwd_hash_params))
        return -1;
    SingleFile check = {fs, store_id, path, native_username, old_file, new_file, 0};
    int result = single_tree(&check, base->root_id, candidate->root_id, "/");
    if (result < 0 || check.changes != 1) {
        old_file[0] = new_file[0] = 0;
        return -1;
    }
    return 0;
}

static int build_file_tree (BuildFile *build, const char *root, char **parts,
                           char new_root[41])
{
    SeafDir *old = seaf_fs_manager_get_seafdir(build->fs, build->store, 1, root);
    SeafDir *updated = NULL;
    GList *entries = NULL;
    const char *previous = NULL;
    gboolean found = FALSE;
    int result = -1;
    if (!old || old->version != 1 || !parts[0] || !*parts[0]) goto out;
    for (GList *item = old->entries; item; item = item->next) {
        SeafDirent *entry = item->data, *copy = NULL;
        if (!entry_valid(entry) || (previous && strcmp(previous, entry->name) <= 0)) goto out;
        previous = entry->name;
        if (strcmp(entry->name, parts[0])) copy = seaf_dirent_dup(entry);
        else {
            if (found) goto out;
            found = TRUE;
            if (!parts[1]) {
                if (!S_ISREG(entry->mode) || entry->version != 1 || strcmp(entry->id, build->old_file)) goto out;
                copy = seaf_dirent_new(1, build->new_file, entry->mode, entry->name,
                    build->mtime, build->username, build->size);
            }
            else {
                char child[41] = {0};
                if (!S_ISDIR(entry->mode) || entry->version != 1 ||
                    build_file_tree(build, entry->id, parts + 1, child) < 0) goto out;
                copy = seaf_dirent_dup(entry);
                memcpy(copy->id, child, 41);
                copy->mtime = build->mtime;
            }
        }
        if (!copy) goto out;
        entries = g_list_prepend(entries, copy);
    }
    if (!found) goto out;
    entries = g_list_reverse(entries);
    updated = seaf_dir_new(NULL, entries, 1);
    entries = NULL; // SeafDir owns the copied entries.
    if (!updated || seaf_dir_save(build->fs, build->store, 1, updated) < 0) goto out;
    memcpy(new_root, updated->dir_id, 41);
    result = 0;
out:
    for (GList *item = entries; item; item = item->next) seaf_dirent_free(item->data);
    g_list_free(entries);
    if (old) seaf_dir_free(old);
    if (updated) seaf_dir_free(updated);
    return result;
}

SeafCommit *cf_build_single_file_candidate (SeafFSManager *fs, SeafCommitManager *commits,
                                           SeafCommit *base, const char *path,
                                           const char *expected_old_file,
                                           const char *indexed_new_file,
                                           const char *native_username)
{
    if (!fs || !commits || !base || base->version != 1 || base->encrypted || !path || path[0] != '/' ||
        !path[1] || strlen(path) > 4096 || !g_utf8_validate(path, -1, NULL) ||
        path[strlen(path)-1] == '/' || strstr(path, "//") || !native_username || !*native_username ||
        !expected_old_file || strlen(expected_old_file) != 40 || strspn(expected_old_file, "0123456789abcdef") != 40 ||
        !indexed_new_file || strlen(indexed_new_file) != 40 || strspn(indexed_new_file, "0123456789abcdef") != 40)
        return NULL;
    Seafile *file = seaf_fs_manager_get_seafile(fs, base->repo_id, 1, indexed_new_file);
    if (!file || file->file_size > G_MAXINT64) { if (file) seafile_unref(file); return NULL; }
    BuildFile build = {fs, base->repo_id, expected_old_file, indexed_new_file, native_username,
                       (gint64)file->file_size, (gint64)(g_get_real_time() / G_USEC_PER_SEC)};
    seafile_unref(file);
    char **parts = g_strsplit(path + 1, "/", -1);
    char root[41] = {0};
    int result = build_file_tree(&build, base->root_id, parts, root);
    g_strfreev(parts);
    if (result < 0) return NULL;
    SeafCommit *candidate = seaf_commit_new(NULL, base->repo_id, root, native_username,
        "0000000000000000000000000000000000000000", "Update local file", 0);
    if (!candidate) return NULL;
    candidate->parent_id = g_strdup(base->commit_id);
    candidate->version = base->version;
    candidate->repo_name = g_strdup(base->repo_name);
    candidate->repo_desc = g_strdup(base->repo_desc);
    candidate->repo_category = g_strdup(base->repo_category);
    candidate->no_local_history = base->no_local_history;
    candidate->enc_version = base->enc_version;
    candidate->magic = g_strdup(base->magic);
    candidate->random_key = g_strdup(base->random_key);
    candidate->salt = g_strdup(base->salt);
    candidate->pwd_hash = g_strdup(base->pwd_hash);
    candidate->pwd_hash_algo = g_strdup(base->pwd_hash_algo);
    candidate->pwd_hash_params = g_strdup(base->pwd_hash_params);
    char old_file[41], new_file[41];
    if (cf_check_single_file_candidate(fs, base->repo_id, base, candidate, path,
            native_username, old_file, new_file) < 0 || strcmp(old_file, expected_old_file) ||
        strcmp(new_file, indexed_new_file) || seaf_commit_manager_add_commit(commits, candidate) < 0) {
        seaf_commit_unref(candidate);
        return NULL;
    }
    // Immutable objects may be unreferenced after a failed CAS. Do not publish
    // here or delete shared content objects; normal GC owns their reclamation.
    return candidate;
}
