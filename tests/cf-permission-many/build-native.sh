#!/usr/bin/env bash
# Optional full Linux build with TEST-ONLY stderr counters, never a release binary.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
server=$(cd "$here/../.." && pwd)
workspace=$(dirname "$server")
configured=${CF_MANY_CONFIGURED_SOURCE:-$workspace/cloudfile-docker/build/cloudfile_14.0/src/seafile-server}
prefix=${CF_MANY_BUILD_PREFIX:-$workspace/cloudfile-docker/build/cloudfile_14.0/seafile-server/seafile}
evhtp=${CF_MANY_EVHTP:-$workspace/cloudfile-docker/build/cloudfile_14.0/src/libevhtp}
output=${CF_MANY_BUILD_OUTPUT:-$(mktemp -d /tmp/cf-search-many-build.XXXXXX)}
mkdir -p "$output"
output=$(cd "$output" && pwd)
test -f "$configured/config.status"
docker run --rm --network none \
    -v "$output:/lab" -v "$server:/source:ro" -v "$configured:/configured:ro" \
    -v "$evhtp:/evhtp:ro" \
    -v "$prefix:/work/build/cloudfile_14.0/seafile-server/seafile:ro" \
    "${CF_MANY_BUILD_IMAGE:-cloudfile-build-base:ce14-v2}" \
    bash /source/tests/cf-permission-many/build-native-inner.sh > "$output/build.log" 2>&1
printf 'TEST-ONLY binary: %s/seaf-server\nBuild log: %s/build.log\n' "$output" "$output"
