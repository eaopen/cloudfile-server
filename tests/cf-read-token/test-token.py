"""Compile production token gates against SQL failures and credential rotation."""
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
USER = (ROOT / 'common/user-mgr.c').read_text()
BRANCH = (ROOT / 'common/branch-mgr.c').read_text()


def function(source, name):
    start = source.index(name + ' (')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fixture = r'''
#include <glib.h>
#include <jansson.h>
#include <string.h>
typedef struct { const char *user; const char *key; } SeafDBRow;
typedef struct { int unused; } SeafDBTrans;
typedef struct { GKeyFile *config; void *user_mgr; } Session;
typedef struct { Session *session; } CcnetUserManager;
typedef struct { Session *seaf; } SeafBranchManager;
static const char *owner = "employee@auth.local";
static const char *stored_key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
static int stored_rows = 1, engine_rows = 1, queries;
static gboolean transactional = TRUE;
static const char *seaf_db_row_get_column_text (SeafDBRow *row, int column) {
    return column == 0 ? row->user : row->key;
}
static gboolean cloudfile_account_engine (SeafDBRow *row, void *data) {
    *(gboolean *)data = transactional; return TRUE;
}
static int seaf_db_trans_foreach_selected_row (SeafDBTrans *trans, const char *sql,
    gboolean (*callback)(SeafDBRow *, void *), void *data, int count, ...) {
    ++queries;
    SeafDBRow row = { owner, stored_key };
    if (strstr(sql, "information_schema")) {
        g_assert_nonnull(strstr(sql, "table_name='api2_token'"));
        if (engine_rows == 1) callback(&row, data);
        return engine_rows;
    }
    g_assert_nonnull(strstr(sql, "`hub`.api2_token"));
    g_assert_nonnull(strstr(sql, "WHERE user=? LIMIT 2 FOR UPDATE"));
    if (stored_rows == 1) callback(&row, data);
    return stored_rows;
}
static int cloudfile_check_oidc_reference (SeafBranchManager *m, SeafDBTrans *t, json_t *r) { return 0; }
static int cloudfile_check_browser_reference (SeafBranchManager *m, SeafDBTrans *t, json_t *r) { return 0; }
static int cloudfile_check_local_reference (SeafBranchManager *m, SeafDBTrans *t,
    const char *repo, const char *path, const char *obj, int kind,
    const char *conditions, const char *user, json_t *root) { return 0; }
'''
start = USER.rfind('typedef struct {', 0, USER.index('} CloudFileReadToken;'))
fixture += USER[start:USER.index('} CloudFileReadToken;') + len('} CloudFileReadToken;')]
fixture += '\nstatic gboolean\n' + function(USER, 'cloudfile_read_token_row')
fixture += '\nint\n' + function(USER, 'ccnet_user_manager_lock_read_token')
fixture += '\nstatic const char *\n' + function(BRANCH, 'cloudfile_reference_text')
fixture += '\nstatic int\n' + function(BRANCH, 'cloudfile_check_session_reference')
fixture += r'''
int main (void) {
    Session session = { g_key_file_new(), NULL };
    CcnetUserManager user_mgr = { &session };
    session.user_mgr = &user_mgr;
    SeafBranchManager mgr = { &session };
    SeafDBTrans tx = { 0 };
    g_key_file_set_string(session.config, "cloudfile", "identity_database", "hub");
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, stored_key, -1);
    json_t *root = json_pack("{s:{s:s}}", "native_read_token", "sha256", digest);
    g_assert_cmpint(cloudfile_check_session_reference(&mgr, &tx, "repo", "/a", "obj", 0, "", owner, root), ==, 0);
    /* Rotation/deletion and wrong owner must fail on the very next check. */
    stored_key = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    g_assert_cmpint(ccnet_user_manager_lock_read_token(&user_mgr, &tx, owner, digest), <, 0);
    stored_key = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    owner = "other@auth.local";
    g_assert_cmpint(ccnet_user_manager_lock_read_token(&user_mgr, &tx, "employee@auth.local", digest), <, 0);
    owner = "employee@auth.local";
    for (int i = -1; i <= 2; ++i) {
        if (i == 1) continue;
        stored_rows = i;
        g_assert_cmpint(ccnet_user_manager_lock_read_token(&user_mgr, &tx, owner, digest), <, 0);
    }
    stored_rows = 1;
    transactional = FALSE;
    g_assert_cmpint(ccnet_user_manager_lock_read_token(&user_mgr, &tx, owner, digest), <, 0);
    transactional = TRUE; engine_rows = -1;
    g_assert_cmpint(ccnet_user_manager_lock_read_token(&user_mgr, &tx, owner, digest), <, 0);
    engine_rows = 1;
    const char *ambiguous[] = {"browser_session", "oidc_session", "local_session", "user_delegation"};
    for (guint i = 0; i < G_N_ELEMENTS(ambiguous); ++i) {
        json_object_set_new(root, ambiguous[i], json_object());
        int before = queries;
        g_assert_cmpint(cloudfile_check_session_reference(&mgr, &tx, "repo", "/a", "obj", 0, "", owner, root), <, 0);
        g_assert_cmpint(queries, ==, before);
        json_object_del(root, ambiguous[i]);
    }
    int before = queries;
    const char *invalid[] = {"", "bad", "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i)
        g_assert_cmpint(ccnet_user_manager_lock_read_token(&user_mgr, &tx, owner, invalid[i]), <, 0);
    g_assert_cmpint(queries, ==, before);
    g_key_file_remove_key(session.config, "cloudfile", "identity_database", NULL);
    g_assert_cmpint(ccnet_user_manager_lock_read_token(&user_mgr, &tx, owner, digest), <, 0);
    json_decref(root); g_free(digest); g_key_file_unref(session.config);
    g_print("Native token rotation, deletion, owner, proof ambiguity and SQL failure checks passed\n");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as temp:
    source, binary = Path(temp) / 'token.c', Path(temp) / 'token'
    source.write_text(fixture)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0', 'jansson'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', str(source), '-o', str(binary), *flags], check=True)
    subprocess.run([str(binary)], check=True)
