/* Execute the production config guard and copy path, with fault-only I/O seams. */
#include "common.h"
#include <fcntl.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include "utils.h"
static int fail_sync, fail_copy, inject_competing_writer;
static const char *destination;
static int test_fsync (int fd) {
    if (fail_sync) { errno = EIO; return -1; }
    return fsync (fd);
}
#define fsync test_fsync
#include "../../common/cf-v2-storage.c"
#undef fsync
SeafileSession *seaf;

/* SQL is the only mocked boundary. Route, allocation, and on-disk checks are
 * the real production C functions included above. */
struct SeafDBRow { const char *cell[5]; };
struct SeafDBTrans { int unused; };
static struct SeafDBTrans fake_trans;
static const char *row_cells[5];
static int fail_insert, fail_commit, sql_inserts, commits, rollbacks, route_selects;

const char *
seaf_db_row_get_column_text (SeafDBRow *row, guint32 col)
{
    g_assert_cmpuint (col, <, 5);
    return ((struct SeafDBRow *)row)->cell[col];
}

int
seaf_db_statement_foreach_row (SeafDB *db, const char *sql,
                               SeafDBRowFunc fn, void *data, int n, ...)
{
    g_assert_nonnull (strstr (sql, "RepoStorageId"));
    route_selects++;
    /* A folder-share sub-library writes its first commit while only the
     * VirtualRepo row exists; the native-existence join must include it. */
    g_assert_nonnull (strstr (sql, "VirtualRepo"));
    struct SeafDBRow row = {0};
    for (int i = 0; i < 5; i++)
        row.cell[i] = row_cells[i];
    fn (&row, data);
    return 1;
}

gboolean
is_uuid_valid (const char *uuid)
{
    return uuid && strlen (uuid) == 36 && uuid[8] == '-' && uuid[13] == '-' &&
           uuid[18] == '-' && uuid[23] == '-';
}

SeafDBTrans *
seaf_db_begin_transaction (SeafDB *db) { return &fake_trans; }

gboolean
seaf_db_trans_check_for_existence (SeafDBTrans *trans, const char *sql,
                                    gboolean *db_err, int n, ...)
{
    *db_err = FALSE;
    return FALSE;
}

int
seaf_db_trans_query (SeafDBTrans *trans, const char *sql, int n, ...)
{
    /* A single transaction writes the native route and the minimal UUID
     * safety marker. Neither of them stores a business alias or active job. */
    g_assert_true (strstr (sql, "INSERT INTO RepoStorageId") != NULL ||
                   strstr (sql, "INSERT INTO cf_library_storage") != NULL);
    sql_inserts++;
    return fail_insert && sql_inserts == fail_insert ? -1 : 0;
}

int
seaf_db_commit (SeafDBTrans *trans)
{
    commits++;
    return fail_commit ? -1 : 0;
}

int
seaf_db_rollback (SeafDBTrans *trans)
{
    rollbacks++;
    return 0;
}

void seaf_db_trans_close (SeafDBTrans *trans) { }

static void
route_row (const char *storage, const char *legacy_key, const char *state,
           gboolean row_exists, gboolean native_exists)
{
    static const char *uuid = "11111111-1111-4111-8111-111111111111";
    row_cells[0] = storage;
    row_cells[1] = row_exists ? uuid : NULL;
    row_cells[2] = legacy_key;
    row_cells[3] = state;
    row_cells[4] = native_exists ? uuid : NULL;
    /* Changes to the fake immutable route model are explicit invalidations. */
    route_cache_forget (uuid);
}

static void
test_uuid_v2_and_legacy_routes (const char *base)
{
    const char *repo = "11111111-1111-4111-8111-111111111111";
    char *root = g_build_filename (base, "libraries", NULL);
    char *native = g_build_filename (base, "native", "storage", "commits", NULL);
    char *current = g_build_filename (root, repo, NULL);
    char *historical = g_build_filename (root, "historical-key", NULL);
    g_assert_cmpint (g_mkdir_with_parents (native, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (root, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (current, 0700), ==, 0);
    g_assert_cmpint (g_mkdir (historical, 0700), ==, 0);
    GKeyFile *config = g_key_file_new ();
    g_key_file_set_string (config, "cloudfile_v2", "auto_local_root", root);
    SeafileSession session = {0};
    session.config = config;
    session.db = (SeafDB *)0x1;
    seaf = &session;

    route_row ("auto-local:11111111-1111-4111-8111-111111111111",
               NULL, NULL, TRUE, FALSE);
    char *actual = cf_v2_store_dir (native, repo);
    char *expected = g_build_filename (current, "storage", "commits", repo, NULL);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);
    g_free (expected);

    /* Thousands of FS/block operations in one library use one positive
     * route lookup. A second library has its own slot and never evicts it. */
    int first_lookups = route_selects;
    for (int i = 0; i < 40; ++i) {
        actual = cf_v2_store_dir (native, repo);
        g_assert_nonnull (actual);
        g_free (actual);
    }
    g_assert_cmpint (route_selects, ==, first_lookups);
    const char *other_repo = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
    actual = cf_v2_store_dir (native, other_repo);
    g_assert_nonnull (actual);
    g_free (actual);
    g_assert_cmpint (route_selects, ==, first_lookups + 1);
    actual = cf_v2_store_dir (native, repo);
    g_assert_nonnull (actual);
    g_free (actual);
    g_assert_cmpint (route_selects, ==, first_lookups + 1);
    g_mutex_lock (&route_cache_mutex);
    CachedRoute *cached = g_hash_table_lookup (route_cache_index, repo);
    g_assert_nonnull (cached);
    cached->expires_at = 0;
    g_mutex_unlock (&route_cache_mutex);
    actual = cf_v2_store_dir (native, repo);
    g_assert_nonnull (actual);
    g_free (actual);
    g_assert_cmpint (route_selects, ==, first_lookups + 2);

    route_row ("auto-local:historical-key", NULL, NULL, TRUE, FALSE);
    actual = cf_v2_store_dir (native, repo);
    expected = g_build_filename (historical, "storage", "commits", repo, NULL);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);
    g_free (expected);

    route_row (NULL, NULL, NULL, FALSE, TRUE);
    actual = cf_v2_store_dir (native, repo);
    expected = g_build_filename (native, repo, NULL);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);

    route_row (NULL, "native-11111111-1111-4111-8111-111111111111",
               "retired", FALSE, TRUE);
    actual = cf_v2_store_dir (native, repo);
    g_assert_cmpstr (actual, ==, expected);
    g_free (actual);
    route_row (NULL, NULL, NULL, FALSE, FALSE);
    int failed_lookups = route_selects;
    g_assert_null (cf_v2_store_dir (native, repo));
    g_assert_null (cf_v2_store_dir (native, repo));
    g_assert_cmpint (route_selects, ==, failed_lookups + 2);
    route_row (NULL, "historical-key", "active", FALSE, TRUE);
    g_assert_null (cf_v2_store_dir (native, repo));
    route_row (NULL, repo, "active", FALSE, TRUE);
    /* Lost new V2 native route is not mistaken for an original CE repo. */
    g_assert_null (cf_v2_store_dir (native, repo));
    route_row (NULL, NULL, NULL, TRUE, TRUE);
    g_assert_null (cf_v2_store_dir (native, repo));
    route_row ("auto-local:missing-dir", NULL, NULL, TRUE, FALSE);
    g_assert_null (cf_v2_store_dir (native, repo));

    g_free (expected);
    g_free (root);
    g_free (native);
    g_free (current);
    g_free (historical);
    g_key_file_free (config);
    seaf = NULL;
}

static void
test_uuid_allocation_faults (const char *base)
{
    const char *repo = "22222222-2222-4222-8222-222222222222";
    char *root = g_build_filename (base, "alloc-root", NULL);
    char *directory = g_build_filename (root, repo, NULL);
    g_assert_cmpint (g_mkdir (root, 0700), ==, 0);
    GKeyFile *config = g_key_file_new ();
    g_key_file_set_string (config, "cloudfile_v2", "auto_local_root", root);
    SeafileSession session = {0};
    session.config = config;
    session.db = (SeafDB *)0x1;
    seaf = &session;

    sql_inserts = commits = rollbacks = 0;
    fail_insert = 1;
    fail_commit = 0;
    g_assert_cmpint (cf_v2_storage_reserve (repo), ==, -1);
    g_assert_cmpint (sql_inserts, ==, 1);
    g_assert_cmpint (rollbacks, ==, 1);
    g_assert_false (g_file_test (directory, G_FILE_TEST_EXISTS));

    /* Either half of the one transaction can fail without publishing a
     * partial route or orphaning the physical UUID directory. */
    const char *other_repo = "33333333-3333-4333-8333-333333333333";
    char *other = g_build_filename (root, other_repo, NULL);
    sql_inserts = commits = rollbacks = 0;
    fail_insert = 2;
    g_assert_cmpint (cf_v2_storage_reserve (other_repo), ==, -1);
    g_assert_cmpint (sql_inserts, ==, 2);
    g_assert_false (g_file_test (other, G_FILE_TEST_EXISTS));

    sql_inserts = commits = rollbacks = 0;
    fail_insert = 0;
    g_assert_cmpint (cf_v2_storage_reserve (other_repo), ==, 0);
    g_assert_cmpint (sql_inserts, ==, 2);
    g_assert_true (g_file_test (other, G_FILE_TEST_IS_DIR));
    g_free (other);

    sql_inserts = commits = rollbacks = 0;
    fail_insert = 0;
    fail_commit = 1;
    g_assert_cmpint (cf_v2_storage_reserve (repo), ==, -1);
    g_assert_cmpint (commits, ==, 1);
    g_assert_true (g_file_test (directory, G_FILE_TEST_IS_DIR));

    /* Uncertain commits cannot be adopted/retried into a different repo. */
    sql_inserts = commits = rollbacks = 0;
    fail_commit = 0;
    g_assert_cmpint (cf_v2_storage_reserve (repo), ==, -1);
    g_assert_cmpint (sql_inserts, ==, 0);

    g_free (directory);
    g_free (root);
    g_key_file_free (config);
    seaf = NULL;
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
    if (inject_competing_writer) {
        /* Simulate a second worker winning between EXDEV and publication. */
        inject_competing_writer = 0;
        g_assert_true (g_file_set_contents (destination, "other writer", -1, NULL));
    }
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
    test_uuid_v2_and_legacy_routes (argv[1]);
    test_uuid_allocation_faults (argv[1]);
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
    inject_competing_writer = 1;
    g_assert_cmpint (cf_v2_storage_copy (src, dst), ==, 0);
    g_assert_cmpint (inject_competing_writer, ==, 0);
    char *first_writer = NULL;
    g_assert_true (g_file_get_contents (dst, &first_writer, NULL, NULL));
    g_assert_cmpstr (first_writer, ==, "other writer");
    g_free (first_writer);
    g_unlink (dst);
    assert_clean (cross);

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
