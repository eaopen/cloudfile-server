#!/usr/bin/env python3
"""Compile-check the seam's call sites without the full seafile build.

server/repo-op.c holds every C write entry point, and it cannot be compiled on
a developer machine -- it needs searpc, jansson, the vala-generated object
headers and the rest of the seafile tree. So the mistakes that designated
initializers make easy (a misspelled field, a stale operation name, a missing
comma that silently turns two arguments into one) would first surface on a
Linux CI run twenty minutes later.

This extracts every CF_FILEOP_* invocation, replaces each value expression
with a dummy of the declared field type, and compiles the result against the
real cf-fileop.h. What survives is exactly the part that does not depend on
seafile: the operation vocabulary, the field names, the arity and the macro
expansion itself.

What it deliberately does NOT check: whether the value passed is the right
variable. `.name = parent_dir` type-checks and is wrong. Only a review or an
end-to-end run catches that.

Usage: check-call-sites.py <cloudfile-server repo root>
"""

import os
import re
import subprocess
import sys
import tempfile

MACROS = ('CF_FILEOP_PREPARE', 'CF_FILEOP_COMMITTED', 'CF_FILEOP_ABORTED')

SOURCES = ('server/repo-op.c',)


def parse_struct_fields(header):
    """Field name -> C type, from the CfFileOp definition."""
    body = re.search(r'typedef struct CfFileOp \{(.*?)\} CfFileOp;',
                     header, re.S)
    if not body:
        sys.exit('could not find the CfFileOp definition in cf-fileop.h')

    fields = {}
    for line in body.group(1).split('\n'):
        line = re.sub(r'/\*.*?\*/', '', line).strip()
        m = re.match(r'^(const char|GList|CfFileOpPhase)\s*(\**)\s*(\w+)\s*;', line)
        if m:
            base, stars, name = m.groups()
            fields[name] = (base + ' ' + stars).strip()
    return fields


def parse_operations(header):
    return set(re.findall(r'#define (CF_OP_\w+)\s', header))


def extract_calls(path):
    """Yield (macro, argument text, line number) for each invocation."""
    with open(path, encoding='utf-8') as fp:
        src = fp.read()

    for macro in MACROS:
        for m in re.finditer(r'\b%s\s*\(' % macro, src):
            start = m.end()
            depth = 1
            i = start
            while i < len(src) and depth:
                if src[i] == '(':
                    depth += 1
                elif src[i] == ')':
                    depth -= 1
                i += 1
            if depth:
                sys.exit('%s: unbalanced parentheses after %s' % (path, macro))
            yield macro, src[start:i - 1], src.count('\n', 0, m.start()) + 1


def split_args(text):
    """Split on top-level commas only."""
    args, depth, current = [], 0, []
    for ch in text:
        if ch in '([':
            depth += 1
        elif ch in ')]':
            depth -= 1
        if ch == ',' and depth == 0:
            args.append(''.join(current).strip())
            current = []
        else:
            current.append(ch)
    if current:
        args.append(''.join(current).strip())
    return args


DUMMY = {'const char *': 'CF_DUMMY_STR', 'GList *': 'CF_DUMMY_LIST'}


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2

    root = sys.argv[1]
    header = open(os.path.join(root, 'common', 'cf-fileop.h'),
                  encoding='utf-8').read()
    fields = parse_struct_fields(header)
    operations = parse_operations(header)

    errors = []
    body = []
    total = 0

    for source in SOURCES:
        path = os.path.join(root, source)
        for macro, argtext, line in extract_calls(path):
            total += 1
            where = '%s:%d' % (source, line)
            args = split_args(argtext)

            if not args:
                errors.append('%s: %s with no arguments' % (where, macro))
                continue

            op = args[0]
            op_expr = op
            if op not in operations:
                # Either a ternary picking between two operations, or a local
                # holding one -- commit_file_blocks uses a local because it
                # reports caller intent and needs the same value three times.
                # Resolve the local to its initializer before checking.
                mentioned = re.findall(r'CF_OP_\w+', op)
                if not mentioned and re.fullmatch(r'\w+', op):
                    decl = re.search(
                        r'const char \*%s\s*=\s*([^;]+);' % re.escape(op),
                        open(path, encoding='utf-8').read())
                    if decl:
                        mentioned = re.findall(r'CF_OP_\w+', decl.group(1))
                        # It is a variable, so the generated program needs a
                        # variable of that type rather than the name itself.
                        op_expr = 'CF_DUMMY_STR'
                if not mentioned:
                    errors.append('%s: %s does not resolve to an operation'
                                  % (where, op))
                    continue
                for name in mentioned:
                    if name not in operations:
                        errors.append('%s: unknown operation %s'
                                      % (where, name))

            rest = args[1:]
            if macro == 'CF_FILEOP_PREPARE':
                if not rest:
                    errors.append('%s: PREPARE with no error argument' % where)
                    continue
                rest = rest[1:]     # the GError ** argument

            given = set()
            rewritten = []
            for arg in rest:
                m = re.match(r'^\.(\w+)\s*=\s*(.*)$', arg, re.S)
                if not m:
                    errors.append('%s: %r is not a designated initializer'
                                  % (where, arg))
                    continue
                name = m.group(1)
                if name not in fields:
                    errors.append('%s: CfFileOp has no field %r'
                                  % (where, name))
                    continue
                if name == 'phase':
                    errors.append('%s: call sites must not set .phase'
                                  % where)
                    continue
                given.add(name)
                rewritten.append('.%s = %s' % (name, DUMMY[fields[name]]))

            # Every operation but upload-blocks produces a commit, so its
            # COMMITTED must say which one. A fact without a version cannot be
            # lined up against the repo-update stream, and the omission is
            # invisible: the consumer just gets an empty string.
            #
            # This rule exists because it happened: copy and move commit through
            # put_dirent_and_commit and move_file_same_repo, two static helpers
            # that used to swallow the id, so both facts shipped without one.
            if (macro == 'CF_FILEOP_COMMITTED'
                    and 'CF_OP_UPLOAD_BLOCKS' not in op
                    and 'commit_id' not in given):
                errors.append('%s: COMMITTED for %s without .commit_id'
                              % (where, op))

            if macro == 'CF_FILEOP_PREPARE':
                body.append('    /* %s */' % where)
                body.append('    if (%s (%s, &cf_error%s) < 0) return 1;'
                            % (macro, op_expr,
                               (', ' + ', '.join(rewritten)) if rewritten else ''))
            else:
                body.append('    /* %s */' % where)
                body.append('    %s (%s%s);'
                            % (macro, op_expr,
                               (', ' + ', '.join(rewritten)) if rewritten else ''))

    if errors:
        for err in errors:
            print('FAIL %s' % err, file=sys.stderr)
        return 1

    program = '\n'.join([
        '/* Generated by tests/cf-fileop/check-call-sites.py. */',
        '#include "cf-fileop.h"',
        'static const char *CF_DUMMY_STR = "x";',
        'static GList *CF_DUMMY_LIST = NULL;',
        'int cf_check_call_sites (void);',
        'int cf_check_call_sites (void)',
        '{',
        '    GError *cf_error = NULL;',
        '    (void)cf_error;',
        ] + body + [
        '    return 0;',
        '}',
        '',
    ])

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, 'call-sites.c')
        with open(src, 'w', encoding='utf-8') as fp:
            fp.write(program)

        cflags = subprocess.run(['pkg-config', '--cflags', 'glib-2.0'],
                                capture_output=True, text=True,
                                check=True).stdout.split()
        result = subprocess.run(
            ['cc', '-std=c99', '-Wall', '-Wextra', '-Werror',
             '-fsyntax-only', src,
             '-I%s' % os.path.join(root, 'common'),
             '-I%s' % os.path.join(root, 'include')] + cflags,
            capture_output=True, text=True)

        if result.returncode != 0:
            print(result.stderr, file=sys.stderr)
            print('FAIL call sites do not compile', file=sys.stderr)
            return 1

    print('cf-fileop: %d call sites in %s type-check'
          % (total, ', '.join(SOURCES)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
