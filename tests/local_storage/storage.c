/* Execute the production config guard and copy path, with fault-only I/O seams. */
#include "common.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include "utils.h"
static int fail_sync, fail_copy;
static const char *destination;
static int test_fsync (int fd) {
    if (fail_sync) { errno = EIO; return -1; }
    return fsync (fd);
}
#define fsync test_fsync
#include "../../common/cf-v2-storage.c"
#undef fsync
SeafileSession *seaf;

/* Exercise the REAL C routing helper, rather than a duplicated mock route.
 * The only fake is its one prepared SELECT result; actual native paths,
 * managed directory checks and fallback decisions are production code. */
struct SeafDBRow { const char *value[5]; };
static const char *route_value[5];

int
seaf_db_statement_foreach_row (SeafDB *db, const char *sql,
                               SeafDBRowFunc callback, void *data, int n, ...)
{
    struct SeafDBRow row = {0};
    g_assert_nonnull (strstr (sql, "LEFT JOIN RepoTrash"));
    g_assert_nonnull (strstr (sql, "LEFT JOIN GarbageRepos"));
    g_assert_cmpint (n, ==, 1);
    for (int i = 0; i < 5; i++)
        row.value[i] = route_value[i];
    callback (&row, data);
    return 1;
}

const char *
seaf_db_row_get_column_text (SeafDBRow *row, guint32 idx)
{
    g_assert_cmpuint (idx, <, 5);
    return ((struct SeafDBRow *)row)->value[idx];
}

gboolean is_uuid_valid (const char *id)
{
    return id && strlen (id) == 36 &&
        g_ascii_isxdigit (id[0]) && id[8] == '-' && id[13] == '-' &&
        id[18] == '-' && id[23] == '-';
}

static void
route_fixture (const char *storage_id, const char *key, const char *state,
               gboolean has_route_row, gboolean has_native_repo)
{
    static const char *uuid = "11111111-1111-4111-8111-111111111111";
    route_value[0] = storage_id;
    route_value[1] = key;
    route_value[2] = state;
    route_value[3] = has_route_row ? uuid : NULL;
    route_value[4] = has_native_repo ? uuid : NULL;
}

static void
test_native_storage_routes (const char *fixture)
{
    const char *repo = "11111111-1111-4111-8111-111111111111";
    char *root = g_build_filename (fixture, "routes-managed", NULL);
    char *native = g_build_filename (fixture, "routes-native", "storage", "commits", NULL);
    char *key_dir = g_build_filename (root, "key-a", NULL);
    g_assert_cmpint (g_mkdir_with_parents (native, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (root, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (key_dir, 0700), ==, 0);
    GKeyFile *cfg = g_key_file_new ();
    g_key_file_set_string (cfg, "cloudfile_v2", "auto_local_root", root);
    SeafileSession session = {0};
    session.config = cfg;
    session.db = (SeafDB *)0x1;
    seaf = &session;

    /* Original Seafile library and retired native object GC keep their path. */
    route_fixture (NULL, NULL, NULL, FALSE, TRUE);
    char *actual = cf_v2_store_dir (native, repo);
    char *expected = g_build_filename (native, repo, NULL);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);
    route_fixture (NULL, "native-11111111-1111-4111-8111-111111111111",
                   "retired", FALSE, TRUE);
    actual = cf_v2_store_dir (native, repo);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);

    /* No UUID in native facts: NEVER write a new native path. */
    route_fixture (NULL, NULL, NULL, FALSE, FALSE);
    g_assert_null (cf_v2_store_dir (native, repo));
    route_fixture (NULL, NULL, NULL, TRUE, TRUE);
    g_assert_null (cf_v2_store_dir (native, repo));

    g_free (expected);

    /* First Commit is written while reserved, before Repo is committed.
     * Virtual files MUST pass the origin store_id here, not the virtual UUID. */
    route_fixture ("auto-local:key-a", "key-a", "reserved", TRUE, FALSE);
    actual = cf_v2_store_dir (native, repo);
    expected = g_build_filename (key_dir, "storage", "commits", repo, NULL);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);
    route_fixture ("auto-local:key-a", "key-a", "retired", TRUE, FALSE);
    actual = cf_v2_store_dir (native, repo);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);
    route_fixture ("auto-local:elsewhere", "key-a", "active", TRUE, TRUE);
    g_assert_null (cf_v2_store_dir (native, repo));
    route_fixture ("auto-local:key-a", "key-a", "active", TRUE, TRUE);
    g_assert_cmpint (g_rmdir (key_dir), ==, 0);
    g_assert_null (cf_v2_store_dir (native, repo));

    seaf = NULL;
    g_key_file_free (cfg);
    g_free (root);
    g_free (native);
    g_free (key_dir);
    g_free (expected);
}

ssize_t readn (int fd, void *buffer, size_t count) { return read (fd, buffer, count); }
ssize_t writen (int output, const void *buffer, size_t count) {
    char *parent = g_path_get_dirname (destination);
    GDir *dir = g_dir_open (parent, 0, NULL);
    const char *name;
    int staged = 0;
    while ((name = g_dir_read_name (dir))) {
        if (g_str_has_prefix (name, ".cf-tmp-object-"))
            staged++;
        g_assert_false (g_str_has_prefix (name, "object."));
    }
    g_dir_close (dir);
    g_free (parent);
    g_assert_cmpint (staged, ==, 1);
    g_assert_false (g_file_test (destination, G_FILE_TEST_EXISTS));
    ssize_t n = write (output, buffer, count);
    if (fail_copy) { errno = EIO; return -1; }
    return n;
}

static void assert_clean (const char *directory) {
    GDir *dir = g_dir_open (directory, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name (dir)))
        g_assert_false (g_str_has_prefix (name, ".cf-tmp-"));
    g_dir_close (dir);
}

static void check_config (const char *body, gboolean accepted) {
    GKeyFile *config = g_key_file_new ();
    g_assert_true (g_key_file_load_from_data (config, body, -1, G_KEY_FILE_NONE, NULL));
    g_assert_cmpint (cf_v2_storage_validate_config (config), ==, accepted ? 0 : -1);
    g_key_file_free (config);
}

int main (int argc, char **argv) {
    const char *sections[] = {"commit_object_backend", "fs_object_backend", "block_backend"};
    const char *backends[] = {"s3", "multiple", "multi", "unknown"};
    g_assert_cmpint (argc, ==, 2);
    test_native_storage_routes (argv[1]);
    check_config ("[storage]\nenable_storage_classes=true\n[block_backend]\nname=s3\n", TRUE);
    check_config ("[cloudfile_v2]\nauto_local_root=/unused\n", TRUE);
    check_config ("[cloudfile_v2]\nauto_local_root=\n[storage]\nenable_storage_classes=true\n", FALSE);
    check_config ("[cloudfile_v2]\nauto_local_root=/unused\n[storage]\nenable_storage_classes=false\n", TRUE);
    for (guint i = 0; i < G_N_ELEMENTS (sections); i++) {
        char *config = g_strdup_printf ("[cloudfile_v2]\nauto_local_root=/unused\n[%s]\nname=filesystem\n", sections[i]);
        check_config (config, TRUE);
        g_free (config);
        for (guint j = 0; j < G_N_ELEMENTS (backends); j++) {
            config = g_strdup_printf ("[cloudfile_v2]\nauto_local_root=/unused\n[%s]\nname=%s\n", sections[i], backends[j]);
            check_config (config, FALSE);
            g_free (config);
        }
    }
    char *src = g_build_filename (argv[1], "source", NULL);
    char *same = g_build_filename (argv[1], "same-volume", NULL);
    g_assert_true (g_file_set_contents (src, "complete payload", -1, NULL));
    g_assert_cmpint (cf_v2_storage_copy (src, same), ==, 0);
    struct stat a, b;
    g_assert_cmpint (stat (src, &a), ==, 0);
    g_assert_cmpint (stat (same, &b), ==, 0);
    g_assert_cmpuint (a.st_ino, ==, b.st_ino);
#ifdef __linux__
    /* tmpfs is an actual second filesystem, not a forced EXDEV return. */
    char *cross = g_strdup ("/dev/shm/cf-storage-copy-XXXXXX");
    g_assert_nonnull (g_mkdtemp (cross));
    g_assert_cmpint (stat (cross, &b), ==, 0);
    g_assert_cmpuint (a.st_dev, !=, b.st_dev);
    char *dst = g_build_filename (cross, "object", NULL);
    destination = dst;
    for (int fault = 0; fault < 3; fault++) {
        fail_sync = fault == 1;
        fail_copy = fault == 2;
        g_assert_cmpint (cf_v2_storage_copy (src, dst), ==, fault ? -1 : 0);
        if (!fault) {
            char *actual = NULL;
            g_assert_true (g_file_get_contents (dst, &actual, NULL, NULL));
            g_assert_cmpstr (actual, ==, "complete payload");
            g_free (actual);
            g_unlink (dst);
        }
        g_assert_false (g_file_test (dst, G_FILE_TEST_EXISTS));
        assert_clean (cross);
    }
    fail_sync = fail_copy = 0;
    g_assert_cmpint (cf_v2_storage_copy ("/does-not-exist", dst), ==, -1);
    assert_clean (cross);
    g_rmdir (cross);
    g_free (cross);
    g_free (dst);
    g_print ("C configuration guard and real cross-filesystem copy/failure cleanup passed\n");
#else
    g_print ("C configuration guard and native hardlink passed; SKIP real cross-filesystem copy (Linux tmpfs required)\n");
#endif
    g_free (same);
    g_free (src);
    return 0;
}
