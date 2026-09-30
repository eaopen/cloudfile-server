#!/usr/bin/env bash
# Compile strict transport plus real scalar/ACL functions; fixture I/O only.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
python3 "$here/compile-fixture.py" "$build"
CF_PERMISSION_MANY_LIBRARY="$build/many.so" python3 "$here/test-many.py" -v
