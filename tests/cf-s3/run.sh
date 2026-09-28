#!/bin/bash

set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo_root=$(cd "$here/../.." && pwd)

if [[ -z ${CF_S3_TEST_ENDPOINT:-} ]]; then
    echo "SKIP C S3 integration test: set CF_S3_TEST_ENDPOINT"
    exit 0
fi

build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT

cc -std=c99 -Wall -Wextra -Wno-unused-parameter \
    -o "$build/test-cf-s3" \
    "$here/test-cf-s3.c" \
    "$repo_root/common/cf-s3-client.c" \
    "$repo_root/common/obj-backend-s3.c" \
    "$repo_root/common/block-backend-s3.c" \
    -I"$repo_root/common" \
    $(pkg-config --cflags --libs glib-2.0 libcurl)

"$build/test-cf-s3"
