#!/bin/bash
# Pure storage policy is testable without the full Seafile runtime.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
cases="${CF_AUTO_STORAGE_CASES:-$root/../cloudfile-docker/docs/features/automatic-local-storage-cases.json}"
build="$(mktemp -d)"
trap 'rm -rf "$build"' EXIT
cc -std=gnu99 -Wall -Wextra -Werror -o "$build/policy" "$here/test-policy.c" \
    -I"$root/common" $(pkg-config --cflags --libs glib-2.0)
# Parse shared JSON in the harness so this pure policy gate needs only glib,
# just like the existing ACL/fileop checks on macOS and Linux.
python3 - "$build/policy" "$cases" <<'PY'
import json
import subprocess
import sys
with open(sys.argv[2]) as stream:
    for case in json.load(stream):
        subprocess.run([sys.argv[1], case['key'], str(case['valid']).lower()], check=True)
PY
echo 'C automatic-local policy fixtures passed'
