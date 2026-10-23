"""Compile production path validators; no server, credentials or file writes.

Run: python3 tests/test_scoped_upload_paths.py
Requires a C compiler, pkg-config and GLib development headers.
This checks path admission, not Hub authorization or end-to-end upload.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "common/rpc-service.c").read_text()


def function(name):
    start = SOURCE.index(name + " (")
    opening = SOURCE.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


# Guard the RPC wiring too: a correct helper is insufficient if uploads still
# use the download predicate that rejects the library root.
assert "!cf_upload_dir_valid (path)" in function("seafile_web_get_scoped_upload_token")
assert "!cf_download_path_valid (path)" in function("seafile_web_get_scoped_download_token")
fixture = "#include <glib.h>\n#include <string.h>\n"
for name in ("cf_download_path_valid", "cf_upload_dir_valid"):
    fixture += "\nstatic gboolean\n" + function(name) + "\n"
fixture += r'''
int main (void) {
    g_assert_true (cf_upload_dir_valid ("/"));
    g_assert_false (cf_download_path_valid ("/"));
    const char *valid[] = {"/folder", "/folder/child", "/中文目录", "/..archive"};
    for (gsize i = 0; i < G_N_ELEMENTS (valid); ++i) {
        g_assert_true (cf_upload_dir_valid (valid[i]));
        g_assert_true (cf_download_path_valid (valid[i]));
    }
    const char *invalid[] = {NULL, "", "relative", "//", "/folder/", "/a//b",
        "/.", "/..", "/a/../b", "/a/./b", "/a\\b", "/a\nb", "/a\x7f", "/\xff"};
    for (gsize i = 0; i < G_N_ELEMENTS (invalid); ++i) {
        g_assert_false (cf_upload_dir_valid (invalid[i]));
        g_assert_false (cf_download_path_valid (invalid[i]));
    }
    char *boundary = g_malloc (4098);
    memset (boundary, 'a', 4097);
    boundary[0] = '/'; boundary[4096] = '\0';
    g_assert_true (cf_upload_dir_valid (boundary));
    g_assert_true (cf_download_path_valid (boundary));
    boundary[4096] = 'a'; boundary[4097] = '\0';
    g_assert_false (cf_upload_dir_valid (boundary));
    g_assert_false (cf_download_path_valid (boundary));
    g_free (boundary);
    g_print ("21 path cases passed for upload and download; RPC wiring checked\n");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="cf-upload-paths-") as temp:
    source = Path(temp) / "paths.c"
    binary = Path(temp) / "paths"
    source.write_text(fixture)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run(shlex.split(os.environ.get("CC", "cc")) +
                   ["-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary)] + flags,
                   check=True)
    subprocess.run([str(binary)], check=True)
