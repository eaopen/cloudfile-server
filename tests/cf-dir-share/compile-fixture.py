#!/usr/bin/env python3
"""Compile the real share-qualification code against a fake Hub channel.

Only the transport is faked. The path canonicalization, the request shape, the
verdict parsing and the r-vs-rw mapping are the shipped code, because those are
exactly the parts a mismatch with the Hub endpoint would silently break: the
wrong JSON key or an ignored `permissions` field would read as "no
qualification" and send a share-only user back to a 403.
"""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build = Path(sys.argv[1]).resolve()
build.mkdir(parents=True, exist_ok=True)
source = (root / 'common/cf-v2-share.c').read_text()


def function(name):
    start = re.search(r'(?m)^(?:static )?char \*\n' + re.escape(name) + r'\s*\(', source)
    if start is None:
        raise ValueError(name)
    body = source.index('{', start.start())
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    depth = 0
    for token in tokens.finditer(source, body):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if not depth:
                return source[start.start():token.end()]
    raise ValueError(name)


# The limits stay single-sourced: copying a magic number into the fixture would
# let the test pass while the shipped bound drifted.
limits = '\n'.join(re.findall(r'(?m)^#define CF_SHARE_\w+ .*$', source))
functions = '\n\n'.join(function(name) for name in
                        ('request_path', 'qualified_permission', 'cf_v2_share_grant'))
(build / 'production.c').write_text(limits + '\n\n' + functions)

cflags = subprocess.check_output(['pkg-config', '--cflags', 'glib-2.0', 'jansson'], text=True).split()
ldflags = subprocess.check_output(['pkg-config', '--libs', 'glib-2.0', 'jansson'], text=True).split()
base = ['cc', '-std=c99', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-fPIC',
        '-I' + str(root / 'common'), '-I' + str(build)]
subprocess.run(base + ['-shared', str(root / 'tests/cf-dir-share/fixture.c'),
                       '-o', str(build / 'share.so'), *cflags, *ldflags], check=True)
