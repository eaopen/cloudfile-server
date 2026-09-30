#!/usr/bin/env bash
# Private native RPC + MySQL fixture. Names/data are never reused from deployments.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
server=$(cd "$here/../.." && pwd)
workspace=$(dirname "$server")
: "${CF_MANY_NATIVE_BINARY:?Set to the TEST-ONLY binary from build-native.sh}"
backend=${CF_MANY_BACKEND:-$workspace/cloudfile-docker/build/cloudfile_14.0/cloudfile-backend}
lab=${CF_MANY_NATIVE_OUTPUT:-$(mktemp -d /tmp/cf-search-many-native.XXXXXX)}
mkdir -p "$lab"
lab=$(cd "$lab" && pwd)
cp "$CF_MANY_NATIVE_BINARY" "$lab/seaf-server"
suffix="$(date +%s)-$$"
network="cf-many-$suffix"; database="cf-many-db-$suffix"
cache="cf-many-redis-$suffix"; runtime="cf-many-runtime-$suffix"
cleanup() {
    docker rm -f "$runtime" "$cache" "$database" >/dev/null 2>&1 || true
    docker network rm "$network" >/dev/null 2>&1 || true
}
trap cleanup EXIT
docker network create "$network" >/dev/null
docker run --rm -d --name "$database" --network "$network" -e MYSQL_ALLOW_EMPTY_PASSWORD=yes mysql:8 >/dev/null
docker run --rm -d --name "$cache" --network "$network" redis:7-alpine >/dev/null
docker run --rm -d --name "$runtime" --network "$network" \
    -e CF_NATIVE_DB_HOST="$database" -e CF_NATIVE_REDIS_HOST="$cache" \
    -e CF_PERMISSION_MANY_ISOLATED=1 \
    -e JWT_PRIVATE_KEY=isolated-search-fixture-only \
    -e SEAFILE_MYSQL_DB_CCNET_DB_NAME=cf_lab_ccnet -e SEAFILE_MYSQL_DB_SEAFILE_DB_NAME=cf_lab_seafile \
    -e LD_LIBRARY_PATH=/backend/seafile-server/seafile/lib:/backend/seafile-server/seafile/lib/seafile \
    -v "$lab:/lab" -v "$backend:/backend:ro" -v "$server:/server:ro" \
    -v "$workspace/cloudfile-hub:/hub:ro" -v "$workspace/cloudfile-docker/tests/e2e:/tests:ro" \
    "${CF_MANY_BUILD_IMAGE:-cloudfile-build-base:ce14-v2}" sleep infinity >/dev/null
ready=false
for attempt in $(seq 1 60); do
    # TCP readiness avoids MySQL's temporary initialization-only Unix socket.
    if docker exec "$runtime" python3 -c 'import os,pymysql;pymysql.connect(host=os.environ["CF_NATIVE_DB_HOST"],user="root",password="").close()' >/dev/null 2>&1; then ready=true; break; fi
    sleep 1
done
test "$ready" = true
docker exec "$runtime" python3 -m pip install --break-system-packages 'django>=4.2,<5' 'djangorestframework>=3.15,<3.16' > "$lab/dependencies.log" 2>&1
docker exec "$runtime" python3 /tests/native_editing_bootstrap.py
docker exec "$runtime" python3 -c 'from pathlib import Path;p=Path("/lab/conf/seafile.conf");p.write_text(p.read_text().replace("file_lock_enabled = true","file_lock_enabled = false\ndir_acl_enabled = true"))'
docker exec -d "$runtime" sh -c '/lab/seaf-server -c /lab/ccnet -d /lab/seafile-data -F /lab/conf -f -l /lab/seafile.log -p /lab/seafile-data > /lab/native-counters.log 2>&1'
ready=false
for attempt in $(seq 1 30); do
    if docker exec "$runtime" test -S /lab/seafile-data/seafile.sock; then ready=true; break; fi
    sleep 1
done
test "$ready" = true
docker exec "$runtime" python3 /server/tests/cf-permission-many/live-native.py | tee "$lab/test.log"
printf 'Native evidence: %s/native-search-results.json\n' "$lab"
