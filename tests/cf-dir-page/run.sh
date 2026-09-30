#!/bin/bash
# Isolated native tests: real scan/RPC/ACL code, fixture storage and sessions.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
python3 "$here/compile-fixture.py" "$build"
export CF_DIR_PAGE_TEST_LIBRARY="$build/page.so"
python3 "$here/test-pages.py" -v
# Exercise the real Hub endpoint using this same compiled native RPC fixture.
hub=${CF_DIR_PAGE_HUB_ROOT:-$(cd "$here/../../.." && pwd)/cloudfile-hub}
if [[ -f "$hub/cloudfile_ext/tests/test_directory_page.py" ]]; then
    PYTHONPATH="$hub${PYTHONPATH:+:$PYTHONPATH}" \
        python3 "$hub/cloudfile_ext/tests/test_directory_page.py" -v
fi
