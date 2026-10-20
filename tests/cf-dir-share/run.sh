#!/usr/bin/env bash
# Compile the real share-qualification code with a fake Hub channel; no network.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
python3 "$here/compile-fixture.py" "$build"
CF_DIR_SHARE_LIBRARY="$build/share.so" python3 "$here/test-share-native.py" -v
