"""Compile the production C boundary with SQL failure/receipt fixtures."""
from pathlib import Path
import shlex
import subprocess
import tempfile

source = (Path(__file__).resolve().parents[2] / 'server/cloudfile-policy.c').read_text()


def function(name):
    start = source.index(name + ' (')
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fixture = r'''
#include <glib.h>
#include <string.h>
#include <stdarg.h>
typedef struct { const char *text; } SeafDBRow;
typedef struct { int unused; } SeafDBTrans;
static const char *receipt, *isolation = "REPEATABLE-READ";
static gboolean valid_shape = TRUE, blocked = FALSE;
static const char *seaf_db_row_get_column_text(SeafDBRow *row, int col) { return row->text; }
static gboolean seaf_db_trans_check_for_existence(SeafDBTrans *tx, const char *sql, gboolean *error, int count, ...) {
    *error = FALSE;
    if (strstr(sql, "FROM cf_schema_migration")) return receipt && strstr(sql, receipt) && strstr(sql, "state='applied'");
    if (strstr(sql, "FROM cf_managed_library WHERE repo_id")) return blocked;
    return valid_shape;
}
static int seaf_db_trans_foreach_selected_row(SeafDBTrans *tx, const char *sql,
    gboolean (*callback)(SeafDBRow *, void *), void *data, int count, ...) {
    if (strstr(sql, "LIMIT 0")) return valid_shape ? 0 : -1;
    if (strstr(sql, "FROM VirtualRepo")) return 0;
    SeafDBRow row = {isolation}; callback(&row, data); return 1;
}
'''
for signature, name in (
    ('static gboolean', 'managed_isolation'), ('static int', 'managed_schema'),
    ('static gboolean', 'managed_origin'), ('static int', 'cf_policy_legacy_library_decision')):
    fixture += '\n' + signature + '\n' + function(name) + '\n'
fixture += r'''
int main(void) {
    SeafDBTrans tx = {0}; gboolean unavailable = FALSE;
    const char *repo = "11111111-1111-4111-8111-111111111111";
    const char *profiles[] = {"032_managed_libraries", "101_m1_native_boundary"};
    for (int i=0; i<2; ++i) {
        receipt=profiles[i]; blocked=FALSE;
        g_assert_cmpint(cf_policy_legacy_library_decision(&tx,repo,&unavailable), ==, 0);
        g_assert_false(unavailable);
        blocked=TRUE;
        g_assert_cmpint(cf_policy_legacy_library_decision(&tx,repo,&unavailable), ==, -1);
        g_assert_false(unavailable);
        valid_shape=FALSE;
        g_assert_cmpint(cf_policy_legacy_library_decision(&tx,repo,&unavailable), ==, -1);
        g_assert_true(unavailable); valid_shape=TRUE; blocked=FALSE;
    }
    receipt=NULL;
    g_assert_cmpint(cf_policy_legacy_library_decision(&tx,repo,&unavailable), ==, -1);
    g_assert_true(unavailable);
    receipt="101_m1_native_boundary"; isolation="READ-COMMITTED";
    g_assert_cmpint(cf_policy_legacy_library_decision(&tx,repo,&unavailable), ==, -1);
    g_assert_true(unavailable);
    g_print("8 C M1/legacy boundary, missing receipt, drift and isolation cases passed\n");
}
'''
with tempfile.TemporaryDirectory(prefix='cf-m1-native-') as directory:
    path, binary = Path(directory) / 'guard.c', Path(directory) / 'guard'
    path.write_text(fixture)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
    subprocess.run(['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', str(path), '-o', str(binary), *flags], check=True)
    subprocess.run([str(binary)], check=True)
