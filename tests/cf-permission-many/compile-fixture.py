#!/usr/bin/env python3
"""Compile actual RPC/scalar/provider/ACL code with fixture I/O and counters.

No production tracing or permission solver is introduced. DB loads are mocked;
these counters are load invocations, not a claim about real SQL statement counts.
"""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]).resolve()
build.mkdir(parents=True, exist_ok=True)

def function(relative, name):
    source = (root / relative).read_text()
    start = re.search(r'(?m)^(?:static )?char \*\n' + re.escape(name) + r'\s*\(', source)
    if start is None: raise ValueError(name)
    body = source.index('{', start.start())
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, body):
        if token.group() == '{': depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth: return source[start.start():token.end()]
    raise ValueError(name)

names = [('common/cf-acl.c', 'cf_acl_apply'), ('common/cf-ext.c', 'cf_ext_check_permission'),
         ('common/rpc-service.c', 'seafile_check_permission'),
         ('common/rpc-service.c', 'seafile_check_permission_by_path'),
         ('common/rpc-service.c', 'seafile_cf_check_permissions_many')]
(build / 'production.c').write_text('\n\n'.join(function(*item) for item in names))
# Compile-only steps must not receive linker inputs: Clang treats unused -l
# flags as errors under -Werror, even when the fixture itself compiles cleanly.
cflags = subprocess.check_output(['pkg-config', '--cflags', 'glib-2.0', 'jansson'], text=True).split()
ldflags = subprocess.check_output(['pkg-config', '--libs', 'glib-2.0', 'jansson'], text=True).split()
base = ['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fPIC',
        '-I' + str(root / 'common'), '-I' + str(build), '-DSEAFILE_SERVER']
# A fake monotonic clock makes timeout coverage deterministic without sleeping.
subprocess.run(base + ['-Dg_get_monotonic_time=fixture_clock', '-c',
    str(root / 'common/cf-permission-many.c'), '-o', str(build / 'many.o'), *cflags], check=True)
subprocess.run(base + ['-shared', str(root / 'tests/cf-permission-many/fixture.c'),
    str(build / 'many.o'), str(root / 'common/cf-acl-resolve.c'), str(root / 'common/cf-path.c'),
    '-o', str(build / 'many.so'), *cflags, *ldflags], check=True)
