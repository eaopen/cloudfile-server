"""Compile real user-mgr qualification against a bounded native SQL fixture."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / 'common/user-mgr.c').read_text()
CASES = Path(os.environ.get('CF_TEST_DOCKER_ROOT', ROOT.parent / 'cloudfile-docker')) / 'tests/cases/system-admin-content-cases.json'


def function(name):
    start = SOURCE.index(name + ' (')
    opening = SOURCE.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (SOURCE[end] == '{') - (SOURCE[end] == '}')
        end += 1
    return SOURCE[start:end]


fixture = r'''
#include <glib.h>
#include <jansson.h>
#include <stdarg.h>
#include <string.h>
typedef struct { const char *role; } SeafDBRow;
typedef struct { int unused; } SeafDBTrans;
typedef struct { void *db; GKeyFile *config; } Session;
typedef struct { void *db; } Private;
typedef struct { Session *session; Private *priv; } CcnetUserManager;
static json_t *current;
static int stored_rows = 1, queries;
static const char *seaf_db_row_get_column_text (SeafDBRow *row, int column) {
    g_assert_cmpint (column, ==, 0); return row->role;
}
static const char *seaf_db_mysql_shared_database (void *db, void *native) { return "native"; }
static int fixture_query (const char *sql, gboolean (*callback)(SeafDBRow *, void *), void *data) {
    ++queries;
    /* Reject loss of the live account gates in either native or final queries. */
    g_assert_nonnull (strstr (sql, "u.is_active=1 AND u.is_staff=1 FOR UPDATE"));
    g_assert_nonnull (strstr (sql, "LEFT JOIN `hub`.role_permissions_adminrole"));
    g_assert_nonnull (strstr (sql, "CASE WHEN r.email IS NULL THEN 'system_admin' ELSE r.role END"));
    if (stored_rows != 1) return stored_rows;
    if (!json_is_true (json_object_get (current, "active")) ||
        !json_is_true (json_object_get (current, "staff"))) return 0;
    SeafDBRow row = { json_is_false (json_object_get (current, "assigned")) ?
        "system_admin" : json_string_value (json_object_get (current, "role")) };
    callback (&row, data); return 1;
}
static int seaf_db_statement_foreach_row (void *db, const char *sql,
    gboolean (*callback)(SeafDBRow *, void *), void *data, int count, ...) {
    return fixture_query (sql, callback, data);
}
static int seaf_db_trans_foreach_selected_row (SeafDBTrans *trans, const char *sql,
    gboolean (*callback)(SeafDBRow *, void *), void *data, int count, ...) {
    g_assert_nonnull (trans); return fixture_query (sql, callback, data);
}
'''
fixture += '\nstatic gboolean\n' + function('cloudfile_library_administrator')
fixture += '\ngboolean\n' + function('ccnet_user_manager_is_global_library_admin')
fixture += r'''
int main (int argc, char **argv) {
    g_assert_cmpint (argc, ==, 2);
    json_t *root = json_load_file (argv[1], 0, NULL);
    g_assert_nonnull (root);
    Session session = { NULL, g_key_file_new () };
    g_key_file_set_string (session.config, "cloudfile", "identity_database", "hub");
    Private priv = { NULL }; CcnetUserManager manager = { &session, &priv };
    SeafDBTrans trans = { 0 };
    size_t index; json_t *cases = json_object_get (root, "cases");
    json_array_foreach (cases, index, current) {
        json_t *overrides = json_object_get (current, "overrides");
        if (overrides) {
            char *text = json_dumps (overrides, JSON_COMPACT);
            g_setenv ("CF_ADMIN_ROLE_PERMISSIONS_JSON", text, TRUE); free (text);
        } else g_unsetenv ("CF_ADMIN_ROLE_PERMISSIONS_JSON");
        gboolean expected = json_is_true (json_object_get (current, "expected"));
        g_assert_cmpint (ccnet_user_manager_is_global_library_admin (&manager, NULL, "admin@example.com"), ==, expected);
        g_assert_cmpint (ccnet_user_manager_is_global_library_admin (&manager, &trans, "admin@example.com"), ==, expected);
        /* A subsequent request must re-read revocation/storage failure, not cache allow. */
        stored_rows = -1;
        g_assert_false (ccnet_user_manager_is_global_library_admin (&manager, &trans, "admin@example.com"));
        stored_rows = 2;
        g_assert_false (ccnet_user_manager_is_global_library_admin (&manager, &trans, "admin@example.com"));
        stored_rows = 1;
    }
    g_assert_cmpint (queries, ==, (int)json_array_size (cases) * 4);
    g_print ("%zu shared role cases passed in native and final-transaction paths\n", json_array_size (cases));
    g_key_file_unref (session.config); json_decref (root); return 0;
}
'''
with tempfile.TemporaryDirectory() as temp:
    source, binary = Path(temp) / 'qualifier.c', Path(temp) / 'qualifier'
    source.write_text(fixture)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0', 'jansson'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Wno-unused-parameter', str(source), '-o', str(binary), *flags], check=True)
    subprocess.run([str(binary), str(CASES)], check=True)
