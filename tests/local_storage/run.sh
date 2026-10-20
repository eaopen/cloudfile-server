#!/usr/bin/env bash
# Compile actual native sources against isolated routing/I/O test seams.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
read -r -a glib_cflags <<< "$(pkg-config --cflags glib-2.0 gobject-2.0)"
read -r -a glib_libs <<< "$(pkg-config --libs glib-2.0 gobject-2.0)"
for name in block object; do
 cc -D_GNU_SOURCE -I"$repo_root/common" -I"$repo_root/lib" "${glib_cflags[@]}" \
  "$repo_root/tests/local_storage/$name.c" "$repo_root/tests/local_storage/stubs.c" \
  "${glib_libs[@]}" -o "$fixture/$name"
 mkdir "$fixture/$name-data"
 "$fixture/$name" "$fixture/$name-data"
done
printf 'Native atomic staging, abort cleanup and enumeration passed\n'
