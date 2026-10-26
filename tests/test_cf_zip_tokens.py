"""Compile the real token manager against isolated native/ACL dependencies.

This exercises issuance, scope storage, redemption and expiry without a live
database or file server. Native subtree policy and real ZIP bytes need DEV QA.
"""
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PRELUDE = r'''
#include <glib.h>
#include <glib-object.h>
#include <jansson.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#define FULL_FEATURE 1
#define SEAFILE_DOMAIN g_quark_from_static_string("zip-test")
#define SEAF_ERR_GENERAL 1
#define SEAF_ERR_BAD_ARGS 2
#define SEAFILE_TYPE_WEB_ACCESS 0
typedef struct { int web_token_expire_time; } SeafileSession;
static SeafileSession session = {7200}, *seaf = &session;
typedef struct { SeafileSession *seaf; struct WebATPriv *priv; } SeafWebAccessTokenManager;
typedef struct { char *scope; char *op; } SeafileWebAccess;
static long now = 1000;
static time_t test_time(time_t *unused) { return now; }
#define time test_time
static void gen_uuid_inplace(char *out) {
    static int counter;
    g_snprintf(out, 37, "00000000-0000-4000-8000-%012d", ++counter);
}
static void ccnet_timer_new(int (*callback)(void *), void *data, int ms) {}
static int cf_v2_acl_unscoped(SeafileSession *s, const char *repo) { return -2; }
static void *test_object_new(int type, ...) {
    SeafileWebAccess *obj = g_new0(SeafileWebAccess, 1);
    va_list ap;
    va_start(ap, type);
    const char *key;
    while ((key = va_arg(ap, const char *))) {
        const char *value = va_arg(ap, const char *);
        if (!strcmp(key, "scope-path")) obj->scope = g_strdup(value);
        if (!strcmp(key, "op")) obj->op = g_strdup(value);
    }
    va_end(ap);
    return obj;
}
#define g_object_new test_object_new
static char *seafile_get_dir_id_by_path(const char *repo, const char *path, GError **error) {
    return strstr(path, "missing") ? NULL : g_strdup("directory-id");
}
static GObject *seafile_get_dirent_by_path(const char *repo, const char *path, GError **error) {
    return strstr(path, "missing") ? NULL : g_object_newv(G_TYPE_OBJECT, 0, NULL);
}
static char *seafile_cf_find_restricted_path(const char *repo, const char *path,
                                           const char *user, GError **error) {
    g_assert_cmpstr(user, ==, "ordinary-user");
    if (strstr(path, "offline")) {
        g_set_error(error, SEAFILE_DOMAIN, SEAF_ERR_GENERAL, "ACL unavailable");
        return NULL;
    }
    return strstr(path, "private") ? g_strdup(path) : NULL;
}
'''

CASES = r'''
static void check(const char *op, const char *body, const char *expected_scope) {
    SeafWebAccessTokenManager *mgr = seaf_web_at_manager_new(seaf);
    GError *error = NULL;
    char *token = seaf_web_at_manager_get_access_token(mgr, "repo", body, op,
                                                       "ordinary-user", 1, &error);
    if (expected_scope) {
        g_assert_no_error(error);
        g_assert_nonnull(token);
        SeafileWebAccess *info = seaf_web_at_manager_query_access_token(mgr, token);
        g_assert_nonnull(info);
        g_assert_cmpstr(info->scope, ==, expected_scope);
        g_assert_cmpstr(info->op, ==, op);
        g_assert_null(seaf_web_at_manager_query_access_token(mgr, token));
        g_free(info->scope); g_free(info->op); g_free(info);
    } else {
        g_assert_null(token);
        g_assert_nonnull(error);
        g_assert_cmpuint(g_hash_table_size(mgr->priv->access_token_hash), ==, 0);
    }
    g_clear_error(&error); g_free(token);
    g_hash_table_destroy(mgr->priv->access_token_hash);
    pthread_mutex_destroy(&mgr->priv->lock); g_free(mgr->priv); g_free(mgr);
}
static void directory(void) {
    check("download-dir", "{\"parent_dir\":\"/\",\"dir_name\":\"docs\",\"obj_id\":\"directory-id\"}", "/docs");
    check("download-dir", "{\"parent_dir\":\"/\",\"dir_name\":\"docs\",\"obj_id\":\"unrelated-id\"}", NULL);
    check("download-dir", "{\"parent_dir\":\"/\",\"dir_name\":\"missing\",\"obj_id\":\"directory-id\"}", NULL);
}
static void multiple(void) {
    check("download-multi", "{\"parent_dir\":\"/文档\",\"file_list\":[\"a +b.xlsx\",\"folder\"]}", "/文档");
    check("download-multi", "{\"parent_dir\":\"/\",\"file_list\":[\"a.xlsx\",\"folder\"]}", "/");
    check("download-multi", "{\"parent_dir\":\"/docs\",\"file_list\":[\"a.xlsx\",\"missing\"]}", NULL);
}
static void denied(void) {
    check("download-multi", "{\"parent_dir\":\"/docs\",\"file_list\":[\"ok\",\"private-subtree\"]}", NULL);
    check("download-dir", "{\"parent_dir\":\"/\",\"dir_name\":\"private\",\"obj_id\":\"directory-id\"}", NULL);
    check("download-multi", "{\"parent_dir\":\"/docs\",\"file_list\":[\"ok\",\"offline\"]}", NULL);
}
static void invalid(void) {
    const char *bodies[] = {
        "{}", "[]", "not json", "{\"parent_dir\":\"/\",\"file_list\":[]}",
        "{\"parent_dir\":\"/a/../b\",\"file_list\":[\"a\"]}",
        "{\"parent_dir\":\"/a//b\",\"file_list\":[\"a\"]}",
        "{\"parent_dir\":\"/a/\",\"file_list\":[\"a\"]}",
        "{\"parent_dir\":\"relative\",\"file_list\":[\"a\"]}",
        "{\"parent_dir\":\"/\",\"file_list\":[\"..\"]}",
        "{\"parent_dir\":\"/\",\"file_list\":[\".\"]}",
        "{\"parent_dir\":\"/\",\"file_list\":[\"\"]}",
        "{\"parent_dir\":\"/\",\"file_list\":[\"a/b\"]}",
        "{\"parent_dir\":\"/\",\"file_list\":[\"a\\\\b\"]}",
        "{\"parent_dir\":\"/\",\"file_list\":[1]}",
        "{\"parent_dir\":\"/\",\"parent_dir\":\"/secret\",\"file_list\":[\"a\"]}"
    };
    for (size_t i = 0; i < G_N_ELEMENTS(bodies); ++i)
        check("download-multi", bodies[i], NULL);
}
static void legacy(void) {
    check("download-dir-link", "{}", NULL);
    check("download-multi-link", "{}", NULL);
    check("download", "file-object-id", NULL);
}
static void expiry(void) {
    SeafWebAccessTokenManager *mgr = seaf_web_at_manager_new(seaf);
    GError *error = NULL;
    char *token = seaf_web_at_manager_get_access_token(mgr, "repo",
        "{\"parent_dir\":\"/\",\"file_list\":[\"a\"]}",
        "download-multi", "ordinary-user", 0, &error);
    g_assert_no_error(error); g_assert_nonnull(token);
    now += 3601;
    g_assert_null(seaf_web_at_manager_query_access_token(mgr, token));
    g_assert_cmpuint(g_hash_table_size(mgr->priv->access_token_hash), ==, 0);
    g_free(token); g_hash_table_destroy(mgr->priv->access_token_hash);
    pthread_mutex_destroy(&mgr->priv->lock); g_free(mgr->priv); g_free(mgr);
}
int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/zip/directory", directory);
    g_test_add_func("/zip/multiple", multiple);
    g_test_add_func("/zip/denied", denied);
    g_test_add_func("/zip/invalid", invalid);
    g_test_add_func("/zip/legacy", legacy);
    g_test_add_func("/zip/expiry", expiry);
    return g_test_run();
}
'''


class NativeZipTokens(unittest.TestCase):
    def test_native_issuance_redemption_and_rejection(self):
        # Source-only CI hosts may lack native headers; never hide a compiler
        # or assertion failure once the required development packages exist.
        if not shutil.which('cc') or not shutil.which('pkg-config'):
            self.skipTest('requires cc, pkg-config, GLib/GObject and Jansson headers')
        available = subprocess.run(
            ['pkg-config', '--exists', 'glib-2.0', 'gobject-2.0', 'jansson'])
        if available.returncode:
            self.skipTest('requires GLib/GObject and Jansson development packages')
        # Replace only external headers/dependencies, not production functions.
        source = (ROOT / 'server/web-accesstoken-mgr.c').read_text()
        source = re.sub(r'^#include .*$', '', source, flags=re.MULTILINE)
        flags = subprocess.check_output(
            ['pkg-config', '--cflags', '--libs', 'glib-2.0', 'gobject-2.0', 'jansson'], text=True)
        with tempfile.TemporaryDirectory(prefix='cf-zip-test-') as tmp:
            cfile, binary = Path(tmp) / 'test.c', Path(tmp) / 'test'
            cfile.write_text(PRELUDE + source + CASES)
            subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra',
                            '-Wno-unused-parameter', '-Wno-unused-variable',
                            '-Wno-deprecated-declarations', str(cfile), '-o', str(binary),
                            *shlex.split(flags), '-pthread'], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
