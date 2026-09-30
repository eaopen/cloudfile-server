#!/usr/bin/env python3
"""Compile verbatim production functions with fake storage, not a fake pager.

The real Vala Dirent, native scan loop, JSON RPC and ACL filter/resolver execute.
Database, storage and session I/O are fixtures; this is not a live RPC test.
"""
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
BUILD = Path(sys.argv[1]).resolve()
BUILD.mkdir(parents=True, exist_ok=True)


def function(relative, name):
    source = (ROOT / relative).read_text()
    start = re.search(r'(?m)^(?:static )?(?:GList \*|json_t \*|char \*|gint)\n' + re.escape(name) + r'\s*\(', source)
    if start is None:
        raise ValueError('Missing production function: ' + name)
    body = source.index('{', start.start())
    # Ignore braces in strings/comments; fail if a source change breaks extraction.
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, body):
        if token.group() == '{': depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[start.start():token.end()]
    raise ValueError('Unclosed production function: ' + name)


functions = [('server/repo-perm.c', 'comp_dirent_func'),
             ('server/repo-perm.c', 'seaf_repo_manager_list_dir_with_perm_page'),
             ('server/repo-perm.c', 'seaf_repo_manager_list_dir_with_perm'),
             ('common/cf-acl.c', 'cf_acl_filter_dirents')]
text = '\n\n'.join(function(path, name) for path, name in functions)
# The registry seam itself is unchanged; this fixture registers the real ACL filter.
text += '\nGList *cf_ext_filter_dirents(const char *r, const char *p, const char *u, GList *d) { return cf_acl_filter_dirents(r, p, u, d); }\n'
text += '\n'.join(function(path, name) for path, name in [
    ('common/cf-dir-page.c', 'dirent_to_json'),
    ('common/cf-dir-page.c', 'cf_list_dir_page_json'),
    ('common/rpc-service.c', 'seafile_cf_list_dir_page')])
(BUILD / 'production-functions.c').write_text(text)
subprocess.run(['valac', '-C', '-H', str(BUILD / 'seafile-object.h'),
                '--directory', str(BUILD), str(ROOT / 'lib/dirent.vala')], check=True)
flags = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0', 'gobject-2.0', 'jansson'], text=True).split()
# Vala-generated C has unrelated warnings; compile it separately, then require
# clean warnings for all tested production functions and the hand-written fixture.
subprocess.run(['cc', '-fPIC', '-w', '-c', str(BUILD / 'dirent.c'), '-o', str(BUILD / 'dirent.o'), *flags], check=True)
subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-shared', '-fPIC', str(ROOT / 'tests/cf-dir-page/fixture.c'), str(BUILD / 'dirent.o'),
                str(ROOT / 'common/cf-acl-resolve.c'), str(ROOT / 'common/cf-path.c'),
                '-I' + str(BUILD), '-I' + str(ROOT / 'common'), '-I' + str(ROOT / 'include'),
                '-o', str(BUILD / 'page.so'), *flags], check=True)
print(BUILD / 'page.so')
