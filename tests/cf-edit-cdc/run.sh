#!/usr/bin/env bash
# Compile only the native Rabin primitive and a deterministic vector probe.
set -euo pipefail
repo_dir="$(cd "$(dirname "$0")/../.." && pwd)"
probe_dir="$(mktemp -d)"
trap 'rm -rf "$probe_dir"' EXIT
cc -O2 -I "$repo_dir/common/cdc" "$repo_dir/common/cdc/rabin-checksum.c" \
  "$repo_dir/tests/cf-edit-cdc/native-vector.c" -o "$probe_dir/native-vector"
"$probe_dir/native-vector"
