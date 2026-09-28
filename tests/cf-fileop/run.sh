#!/bin/bash
#
# Run the C write-lifecycle seam against the shared case set.
#
# Needs only glib, not the full seafile build, because cf-fileop.c and
# cf-path.c are deliberately free of database and session dependencies.
#
# The case file lives in the cloudfile-docker repo. Point CF_FILEOP_CASES at
# it, or check the repos out side by side and the default path works.

set -e

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)
workspace=$(dirname "$repo_root")

cases=${CF_FILEOP_CASES:-$workspace/cloudfile-docker/docs/fileop-cases.json}

if [[ ! -f $cases ]]; then
    echo "shared fileop case set not found at $cases; set CF_FILEOP_CASES" >&2
    exit 1
fi

build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

python3 "$here/gen-cases.py" "$cases" > "$build/cf-fileop-cases.h"

cc -std=c99 -Wall -Wextra -Wno-unused-parameter -o "$build/test-cf-fileop" \
    "$here/test-cf-fileop.c" \
    "$repo_root/common/cf-fileop.c" \
    "$repo_root/common/cf-path.c" \
    -I"$repo_root/common" -I"$repo_root/include" -I"$build" \
    $(pkg-config --cflags --libs glib-2.0)

"$build/test-cf-fileop"

# The seam's call sites live in server/repo-op.c, which cannot be compiled
# without the full seafile build. This checks the part of them that can be:
# that every CF_FILEOP_* invocation names real struct fields, passes a real
# operation and has the right arity. Those are the mistakes a designated
# initializer makes easy, and the ones a Linux-only build would otherwise be
# the first to catch.
python3 "$here/check-call-sites.py" "$repo_root"
