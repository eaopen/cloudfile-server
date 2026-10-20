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
