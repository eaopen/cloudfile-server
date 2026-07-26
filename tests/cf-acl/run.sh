#!/bin/bash
#
# Run the C directory-ACL resolver against the shared case set.
#
# Needs only glib, not the full seafile build, because cf-acl-resolve.c is
# deliberately free of database and session dependencies.
#
# The case file lives in the cloudfile-docker repo. Point CF_ACL_CASES at it,
# or check the repos out side by side and the default path works.

set -e

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)
workspace=$(dirname "$repo_root")

cases=${CF_ACL_CASES:-$workspace/cloudfile-docker/docs/acl-cases.json}

if [[ ! -f $cases ]]; then
    echo "shared ACL case set not found at $cases; set CF_ACL_CASES" >&2
    exit 1
fi

build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

python3 "$here/gen-cases.py" "$cases" > "$build/cf-acl-cases.h"

# cf-path.c holds the path normalization that used to live in
# cf-acl-resolve.c. It moved down to the baseline when the write lifecycle
# seam needed the same rules -- see common/cf-path.h for why one
# implementation rather than two.
cc -std=c99 -Wall -Wextra -Wno-unused-parameter -o "$build/test-cf-acl" \
    "$here/test-cf-acl.c" \
    "$repo_root/common/cf-acl-resolve.c" \
    "$repo_root/common/cf-path.c" \
    -I"$repo_root/common" -I"$build" \
    $(pkg-config --cflags --libs glib-2.0)

"$build/test-cf-acl"
