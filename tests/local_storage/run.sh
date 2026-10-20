#!/usr/bin/env bash
# Compile actual native sources against isolated routing/I/O test seams.
set -euo pipefail
repo_root=$(cd "$(dirname "$0")/../.." && pwd)
fixture=$(mktemp -d)
trap 'rm -rf "$fixture"' EXIT
read -r -a glib_cflags <<< "$(pkg-config --cflags glib-2.0 gobject-2.0)"
read -r -a glib_libs <<< "$(pkg-config --libs glib-2.0 gobject-2.0)"
for name in block object; do
 # The test-only include directory supplies only the socket typedef needed
 # by lib/utils.h; production continues using the real libevent headers.
 cc -D_GNU_SOURCE -I"$repo_root/tests/local_storage/include" -I"$repo_root/common" -I"$repo_root/lib" "${glib_cflags[@]}" \
  "$repo_root/tests/local_storage/$name.c" "$repo_root/tests/local_storage/stubs.c" \
  "${glib_libs[@]}" -o "$fixture/$name"
 mkdir "$fixture/$name-data"
 "$fixture/$name" "$fixture/$name-data"
 # Run the identical native code through the V2 managed-library route, too.
 mkdir "$fixture/$name-managed-data" "$fixture/$name-managed-root"
 CF_TEST_V2_STORAGE_ROOT="$fixture/$name-managed-root" \
     "$fixture/$name" "$fixture/$name-managed-data"
done
printf 'Native and managed C object staging, cleanup and enumeration passed\n'
