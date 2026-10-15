/* I/O fixture only: production scanning, RPC and ACL functions are included
 * by compile-fixture.py, so these stubs never implement paging or filtering. */
#include "common.h"
#include <glib-object.h>
#include <jansson.h>
#include <sys/stat.h>
#include "seafile-object.h"
#include "cf-dir-page.h"
#include "seafile-error.h"

#define SEAFILE_DOMAIN (g_quark_from_static_string("seafile"))
#define seaf_debug(...) ((void)0)

typedef struct { char *id, *name, *modifier; int mode, version; gint64 mtime, size; } SeafDirent;
typedef struct { GList *entries; } SeafDir;
typedef struct { char *store_id; int version; void *virtual_info; } SeafRepo;
typedef struct { int unused; } SeafRepoManager;
static struct { SeafRepoManager *repo_mgr; void *fs_mgr, *share_mgr; } session;
static __typeof__(session) *seaf = &session;
static int total, invalid_index = -1;
static gboolean denied, read_failed, cf_acl_on = TRUE, managed;
static char current_revision[41];
static GHashTable *hidden;
/* 2026-10-15 v0.9-only: per-entry ACL hiding retired with the v0.8 reader.
 * "hidden" entries now carry invalid object ids, which the real scan still
 * counts (scanned_count) but never returns -- the same engine contract the
 * shared pagination cases pin, driven by the only remaining skip cause. */

void fixture_reset (int count)
{
    total = count;
    invalid_index = -1;
    denied = read_failed = managed = FALSE;
    memset (current_revision, 'a', 40);
    current_revision[40] = 0;
    if (hidden) g_hash_table_destroy (hidden);
    hidden = g_hash_table_new (g_direct_hash, g_direct_equal);
}
void fixture_hide (int index, int value)
{
    if (value) g_hash_table_add (hidden, GINT_TO_POINTER(index + 1));
    else g_hash_table_remove (hidden, GINT_TO_POINTER(index + 1));
}
void fixture_set_revision (const char *revision) { g_strlcpy(current_revision, revision, 41); }
void fixture_deny_parent (int value) { denied = value; }
void fixture_manage (int value) { managed = value; }
void fixture_fail_read (int value) { read_failed = value; }
void fixture_invalid (int index) { invalid_index = index; }

static gboolean is_uuid_valid (const char *id) { return g_uuid_string_is_valid(id); }
static gboolean is_object_id_valid (const char *id)
{
    if (!id || strlen(id) != 40) return FALSE;
    for (int i = 0; i < 40; i++) if (!g_ascii_isxdigit(id[i])) return FALSE;
    return TRUE;
}
static char *format_dir_path (const char *path) { return g_strdup(path); }
static char *seaf_repo_manager_check_permission (SeafRepoManager *mgr, const char *repo, const char *user, GError **error)
{ return g_strdup("rw"); }
/* Mirrors the production seam: the parent gate refuses a managed library
 * (real cf_acl_apply returns NULL) before any entry is filtered. The optional
 * unavailable flag stays untouched here: this fixture models a deliberate deny. */
static char *cf_ext_check_permission (const char *repo, const char *path, const char *user,
                                      const char *perm, gboolean *unavailable)
{ (void)unavailable; return (denied || managed) ? NULL : g_strdup(perm); }
static char *seafile_get_dir_id_by_path (const char *repo, const char *path, GError **error)
{ return g_strdup(current_revision); }
static SeafRepo *seaf_repo_manager_get_repo (SeafRepoManager *mgr, const char *id)
{
    SeafRepo *repo = g_new0(SeafRepo, 1);
    repo->store_id = g_strdup(id); repo->version = 1;
    return repo;
}
static void seaf_repo_unref (SeafRepo *repo) { g_free(repo->store_id); g_free(repo); }
static char *seaf_repo_manager_get_repo_owner (SeafRepoManager *mgr, const char *id) { return NULL; }
static GHashTable *seaf_share_manager_get_shared_sub_dirs (void *mgr, const char *repo, const char *path) { return NULL; }
static SeafDir *seaf_fs_manager_get_seafdir (void *mgr, const char *repo, int version, const char *revision)
{
    if (read_failed) return NULL;
    SeafDir *dir = g_new0(SeafDir, 1);
    for (int i = 0; i < total; i++) {
        SeafDirent *entry = g_new0(SeafDirent, 1);
        entry->id = g_strdup((i == invalid_index ||
                              g_hash_table_contains(hidden, GINT_TO_POINTER(i + 1)))
                             ? "invalid" : current_revision);
        entry->name = g_strdup_printf("d%05d", i);
        entry->mode = S_IFDIR | 0755; entry->version = 1;
        entry->mtime = G_GINT64_CONSTANT(9000000000000);
        entry->size = G_GINT64_CONSTANT(8000000000000);
        entry->modifier = g_strdup("user");
        dir->entries = g_list_prepend(dir->entries, entry);
    }
    return dir;
}
static void seaf_dir_free (SeafDir *dir)
{
    for (GList *p = dir->entries; p; p = p->next) {
        SeafDirent *d = p->data;
        g_free(d->id); g_free(d->name); g_free(d->modifier); g_free(d);
    }
    g_list_free(dir->entries); g_free(dir);
}
/* 2026-10-15 v0.9-only: the retired v0.8 rule loader is replaced by the
 * managed-library boundary; the fixture flips it per test case. The optional
 * unavailable flag stays FALSE: this fixture models the deliberate boundary. */
gboolean cf_acl_legacy_entry_denied (const char *repo, gboolean *unavailable)
{
    if (unavailable) *unavailable = FALSE;
    return managed;
}

#include "production-functions.c"

/* Surface GError as a test transport error, never a successful empty page. */
char *fixture_page (const char *request)
{
    GError *error = NULL;
    char *result = seafile_cf_list_dir_page(request, &error);
    if (error) {
        g_assert_null(result);
        result = g_strdup_printf("{\"rpc_error\":%d}", error->code);
        g_error_free(error);
    }
    return result;
}
void fixture_free (void *result) { g_free(result); }
/* Direct probe of the real per-entry filter (the page path denies at the
 * parent gate first, so this is the only coverage of the filter's own
 * managed branch): returns how many entries survive. */
int fixture_filter_count (void)
{
    GList *entries = NULL;
    for (int i = 0; i < 3; i++) {
        entries = g_list_prepend (entries, g_object_new (SEAFILE_TYPE_DIRENT,
            "obj_id", current_revision, "obj_name", "f00000",
            "mode", (int)(S_IFREG | 0644), "version", 1,
            "permission", "rw", NULL));
    }
    GList *kept = cf_ext_filter_dirents ("11111111-1111-4111-8111-111111111111",
                                         "/", "user", entries);
    int count = g_list_length (kept);
    if (kept) g_list_free_full (kept, g_object_unref);
    return count;
}
int fixture_legacy_count (void)
{
    GError *error = NULL;
    GList *entries = seaf_repo_manager_list_dir_with_perm(NULL, "repo", "/", current_revision, "user", -1, -1, &error);
    g_assert_no_error(error);
    int count = g_list_length(entries);
    g_list_free_full(entries, g_object_unref);
    return count;
}
