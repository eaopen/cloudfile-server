"""Diagnostic CE commit probe against explicitly isolated MySQL and new binaries.

This demonstrates baseline behavior, not CloudFile commit protection. It creates
only random test schemas/accounts and temporary libraries; no existing DB names,
credentials, repository IDs or server endpoints are accepted.
"""

import argparse
from contextlib import ExitStack, contextmanager
from concurrent.futures import ThreadPoolExecutor
import json
import os
from pathlib import Path
import secrets
import socket
import subprocess
import tempfile
import threading
import time
import urllib.request
from uuid import uuid4


def wait_ready(check, process):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError("native probe service stopped before readiness")
        try:
            return check()
        except Exception:
            time.sleep(0.1)
    raise RuntimeError("native probe readiness timed out")


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def web_update(http, port, token, head, content):
    boundary = "cfprobe" + uuid4().hex
    body = b""
    for name, value in {"target_file": "/probe.txt", "head": head}.items():
        body += ("--" + boundary + '\r\nContent-Disposition: form-data; name="' + name
                 + '"\r\n\r\n' + value + "\r\n").encode()
    body += ("--" + boundary + '\r\nContent-Disposition: form-data; name="file"; filename="probe.txt"'
             + "\r\nContent-Type: application/octet-stream\r\n\r\n").encode() + content + b"\r\n"
    body += ("--" + boundary + "--\r\n").encode()
    request = urllib.request.Request("http://127.0.0.1:" + str(port) + "/update-api/" + token,
                                     data=body, headers={"Content-Type": "multipart/form-data; boundary=" + boundary})
    with http.open(request, timeout=15) as response:
        return response.status, response.read(1024).decode()


def check_gc_race(api, repo, actor, data, path, admin, database, native_user):
    """Wait for the actual native transaction to block, then advance GC.

    No timing-only success: performance_schema must prove a GCID row-lock
    wait by this probe's unique native SQL account before the race proceeds.
    """
    from pysearpc import SearpcError
    from seafile import ServerThreadedRpcClient
    expected = api.get_repo(repo).head_cmmt_id
    file_id = api.get_file_id_by_path(repo, "/probe.txt")
    with admin.cursor() as cursor:
        cursor.execute("INSERT INTO " + database + ".GCID (repo_id,gc_id) VALUES (%s,%s) "
                       "ON DUPLICATE KEY UPDATE gc_id=VALUES(gc_id)", (repo, uuid4().hex))
    blocker = admin.__class__(host=admin.host, port=admin.port, user="root", password="",
                              autocommit=False, connect_timeout=5, read_timeout=10)
    def write():
        client = ServerThreadedRpcClient(str(data / "seafile.sock"))
        try:
            client.cloudfile_put_file_if_head(repo, str(path), "/", "probe.txt", actor, expected)
            return False
        except SearpcError:
            return True
    try:
        with blocker.cursor() as cursor:
            cursor.execute("SELECT gc_id FROM " + database + ".GCID WHERE repo_id=%s FOR UPDATE", (repo,))
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(write)
            try:
                deadline = time.monotonic() + 10
                while True:
                    with admin.cursor() as cursor:
                        cursor.execute(
                            "SELECT COUNT(*) FROM performance_schema.data_lock_waits w "
                            "JOIN performance_schema.data_locks l ON l.ENGINE_LOCK_ID=w.REQUESTING_ENGINE_LOCK_ID "
                            "JOIN performance_schema.threads t ON t.THREAD_ID=l.THREAD_ID "
                            "WHERE l.OBJECT_SCHEMA=%s AND l.OBJECT_NAME='GCID' AND t.PROCESSLIST_USER=%s",
                            (database, native_user))
                        waiting = cursor.fetchone()[0]
                    if waiting:
                        break
                    if pending.done() or time.monotonic() >= deadline:
                        raise RuntimeError("native GC row-lock wait was not observed")
                    time.sleep(0.02)
                with blocker.cursor() as cursor:
                    cursor.execute("UPDATE " + database + ".GCID SET gc_id=%s WHERE repo_id=%s", (uuid4().hex, repo))
                blocker.commit()
                if not pending.result(timeout=10):
                    raise RuntimeError("strict writer accepted concurrent GC")
            finally:
                # Release the lock even when observation fails before joining.
                blocker.rollback()
        if api.get_repo(repo).head_cmmt_id != expected or api.get_file_id_by_path(repo, "/probe.txt") != file_id:
            raise RuntimeError("rejected GC race changed published content")
    finally:
        blocker.close()


def check_strict_primitive(api, repo, actor, root, data, original, changed, stale, admin, database, native_user):
    from pysearpc import SearpcError
    from seafile import ServerThreadedRpcClient
    client = ServerThreadedRpcClient(str(data / "seafile.sock"))
    def rejected(path, expected, repo_id=repo):
        try:
            client.cloudfile_put_file_if_head(repo_id, str(path), "/", "probe.txt", actor, expected)
        except SearpcError:
            return True
        return False
    base = api.get_repo(repo).head_cmmt_id
    result = client.cloudfile_put_file_if_head(repo, str(original), "/", "probe.txt", actor, base)
    current = api.get_repo(repo).head_cmmt_id
    assert current != base and result == api.get_file_id_by_path(repo, "/probe.txt")
    assert rejected(changed, base) and rejected(original, base)
    for invalid in (None, "", "*", "x" * 40):
        assert rejected(stale, invalid)
    assert api.get_repo(repo).head_cmmt_id == current
    check_gc_race(api, repo, actor, data, changed, admin, database, native_user)
    check_gc_race(api, repo, actor, data, original, admin, database, native_user)
    assert client.cloudfile_put_file_if_head(repo, str(original), "/", "probe.txt", actor, current) == result
    assert api.get_repo(repo).head_cmmt_id == current
    api.set_repo_status(repo, 1)
    assert rejected(changed, current)
    api.set_repo_status(repo, 0)

    barrier = threading.Barrier(2)
    def write(index):
        path = root / ("strict-concurrent-" + str(index))
        path.write_bytes(bytes([index]) * (2 * 1024 * 1024))
        connection = ServerThreadedRpcClient(str(data / "seafile.sock"))
        barrier.wait(timeout=5)
        try:
            connection.cloudfile_put_file_if_head(repo, str(path), "/", "probe.txt", actor, current)
            return True
        except SearpcError:
            return False
    with ThreadPoolExecutor(max_workers=2) as pool:
        winners = sum(pool.map(write, (1, 2)))
    assert winners == 1
    api.post_dir(repo, "/", "virtual-dir", actor)
    virtual = api.create_virtual_repo(repo, "/virtual-dir", "probe virtual", "isolated", actor)
    assert rejected(changed, api.get_repo(virtual).head_cmmt_id, virtual)
    return {"strict_current_head_write": True, "strict_stale_write_and_noop_rejected": True,
            "strict_invalid_head_rejected": True, "strict_current_noop_preserves_head": True,
            "strict_readonly_and_virtual_rejected": True, "strict_concurrent_winners": winners,
            "strict_gc_race_write_and_noop_rejected": True}


def wait_sql(admin, sql, params, pending, *, seconds=3):
    deadline = time.monotonic() + seconds
    while True:
        with admin.cursor() as cursor:
            cursor.execute(sql, params)
            row = cursor.fetchone()
        if row and row[0]:
            return row[0]
        if pending.done() or time.monotonic() >= deadline:
            raise RuntimeError("expected native/worker SQL lock wait was not observed")
        time.sleep(0.02)


def check_barrier_primitive(api, repo, actor, data, original, changed, admin, database, native_user):
    """Actual Hub JobStore and C branch transaction, not fixture publication.

    Completion proof below is explicitly a fixture: account/context/projection
    reconciliation is not claimed. Scopes are privileged inputs, not user grants.
    """
    import pymysql
    from pysearpc import SearpcError
    from seafile import ServerThreadedRpcClient
    from cloudfile_extensions.jobs.authority import canonical_scope, lock_name, scope_locks
    from cloudfile_extensions.jobs.store import BarrierProof, JobStore
    from cloudfile_extensions.schema.runner import SchemaRunner
    options = dict(host=admin.host, port=admin.port, user="root", password="", database=database,
                   autocommit=True, charset="utf8mb4", connect_timeout=5, read_timeout=10)
    scopes = [{"type": "repo", "provider": "cloudfile", "external_id": repo},
              {"type": "user", "provider": "directory", "external_id": "员工:a:b"}]
    def write(path, head, selected=scopes):
        client = ServerThreadedRpcClient(str(data / "seafile.sock"))
        try:
            result = client.cloudfile_put_file_with_barriers(repo, str(path), "/", "probe.txt", actor,
                json.dumps({"head_id": head, "scopes": selected}, ensure_ascii=False))
            return result
        except SearpcError:
            return None
    api.put_file(repo, str(original), "/", "probe.txt", actor, api.get_repo(repo).head_cmmt_id)
    initial = api.get_repo(repo).head_cmmt_id
    assert write(changed, initial) is None  # Missing barrier schema fails closed.
    assert api.get_repo(repo).head_cmmt_id == initial
    with ExitStack() as cleanup:
        connection = pymysql.connect(**options)
        cleanup.callback(connection.close)
        SchemaRunner(connection).apply()
        with connection.cursor() as cursor:
            cursor.execute("INSERT IGNORE INTO GCID(repo_id,gc_id) VALUES(%s,%s)", (repo, uuid4().hex))
        store = JobStore(connection)
        assert write(original, initial) == api.get_file_id_by_path(repo, "/probe.txt")
        assert write(changed, initial) == api.get_file_id_by_path(repo, "/probe.txt")
        assert write(original, api.get_repo(repo).head_cmmt_id)
        for selected in ([scopes[0]], [scopes[1], {**scopes[0], "external_id": str(uuid4())}],
                         [scopes[0], {**scopes[1], "unexpected": "x"}],
                         [scopes[0], {**scopes[1], "external_id": "bad\x00id"}]):
            assert write(changed, api.get_repo(repo).head_cmmt_id, selected) is None

        def submit(scope, key):
            return store.submit(actor="admin", actor_kind="user", kind="authorization.refresh", scope=scope,
                                request={}, idempotency_key=key, barrier=True)[0]
        @contextmanager
        def reconciliation_fixture(claim):
            yield BarrierProof(claim.job_id, claim.epoch)
        # Already-durable barrier rejects content changes and same-content no-op,
        # even after failure/cancel. Other users are not globally frozen.
        job = submit(scopes[1], "subject-barrier")
        head = api.get_repo(repo).head_cmmt_id
        assert write(changed, head) is None and write(original, head) is None
        other = [scopes[0], {**scopes[1], "external_id": "other-user"}]
        assert write(original, head, other)
        claim = store.claim("probe-worker", kinds=("authorization.refresh",))
        store.fail(claim, code="PROBE_FAILED")
        store.cancel(job, actor="admin", actor_kind="user")
        assert write(original, head) is None
        store.retry(job, actor="admin", actor_kind="user")
        claim = store.claim("probe-worker", kinds=("authorization.refresh",))
        store.complete(claim, barrier_guard=reconciliation_fixture)
        assert write(original, head)

        # Read-only wins during indexing/commit: confirm the native transaction
        # is waiting on RepoInfo, then change its status on the blocking SQL
        # connection. Early in-memory status was writable; final read must deny.
        blocker = pymysql.connect(**{**options, "autocommit": False})
        cleanup.callback(blocker.close)
        with blocker.cursor() as cursor:
            cursor.execute("SELECT status FROM RepoInfo WHERE repo_id=%s FOR UPDATE", (repo,))
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(write, changed, head)
            try:
                wait_sql(admin,
                    "SELECT COUNT(*) FROM performance_schema.data_lock_waits w "
                    "JOIN performance_schema.data_locks l ON l.ENGINE_LOCK_ID=w.REQUESTING_ENGINE_LOCK_ID "
                    "JOIN performance_schema.threads t ON t.THREAD_ID=l.THREAD_ID "
                    "WHERE l.OBJECT_SCHEMA=%s AND l.OBJECT_NAME='RepoInfo' AND t.PROCESSLIST_USER=%s",
                    (database, native_user), pending)
                with blocker.cursor() as cursor:
                    cursor.execute("UPDATE RepoInfo SET status=1 WHERE repo_id=%s", (repo,))
                blocker.commit()
            finally:
                blocker.rollback()
            assert pending.result(timeout=10) is None
        assert api.get_repo(repo).head_cmmt_id == head
        api.set_repo_status(repo, 0)
        assert write(original, head)

        # Barrier wins: native must wait for the exact Hub scope lock. Publish
        # the barrier on that same SQL connection before releasing ownership.
        user_name = lock_name(database, canonical_scope(scopes[1]))
        with ThreadPoolExecutor(max_workers=1) as pool:
            with scope_locks(connection, [scopes[1]]):
                pending = pool.submit(write, changed, head)
                wait_sql(admin,
                    "SELECT COUNT(*) FROM performance_schema.metadata_locks l "
                    "JOIN performance_schema.threads t ON t.THREAD_ID=l.OWNER_THREAD_ID "
                    "WHERE l.OBJECT_TYPE='USER LEVEL LOCK' AND l.OBJECT_NAME=%s "
                    "AND l.LOCK_STATUS='PENDING' AND t.PROCESSLIST_USER=%s", (user_name, native_user), pending)
                job = submit(scopes[1], "barrier-wins")
            assert pending.result(timeout=10) is None
        assert api.get_repo(repo).head_cmmt_id == head
        claim = store.claim("probe-worker", kinds=("authorization.refresh",))
        store.complete(claim, barrier_guard=reconciliation_fixture)

        # Publication wins: pause the real native transaction at GCID *after*
        # it acquired all authority scopes. Hub cannot establish its repo barrier
        # until native publication committed and released those scopes.
        blocker = pymysql.connect(**{**options, "autocommit": False})
        cleanup.callback(blocker.close)
        with blocker.cursor() as cursor:
            cursor.execute("SELECT gc_id FROM GCID WHERE repo_id=%s FOR UPDATE", (repo,))
        job_connection = pymysql.connect(**options)
        cleanup.callback(job_connection.close)
        with job_connection.cursor() as cursor:
            cursor.execute("SELECT CONNECTION_ID()")
            job_owner = cursor.fetchone()[0]
        try:
            with ThreadPoolExecutor(max_workers=2) as pool:
                native = pool.submit(write, changed, head)
                wait_sql(admin,
                    "SELECT t.PROCESSLIST_ID FROM performance_schema.data_lock_waits w "
                    "JOIN performance_schema.data_locks l ON l.ENGINE_LOCK_ID=w.REQUESTING_ENGINE_LOCK_ID "
                    "JOIN performance_schema.threads t ON t.THREAD_ID=l.THREAD_ID "
                    "WHERE l.OBJECT_SCHEMA=%s AND l.OBJECT_NAME='GCID' AND t.PROCESSLIST_USER=%s",
                    (database, native_user), native)
                job_store = JobStore(job_connection)
                job_future = pool.submit(job_store.submit, actor="admin", actor_kind="user",
                    kind="authorization.refresh", scope=scopes[0], request={}, idempotency_key="publication-wins", barrier=True)
                try:
                    wait_sql(admin,
                        "SELECT COUNT(*) FROM performance_schema.metadata_locks l "
                        "JOIN performance_schema.threads t ON t.THREAD_ID=l.OWNER_THREAD_ID "
                        "WHERE l.OBJECT_TYPE='USER LEVEL LOCK' AND l.OBJECT_NAME=%s "
                        "AND l.LOCK_STATUS='PENDING' AND t.PROCESSLIST_ID=%s",
                        (lock_name(database, canonical_scope(scopes[0])), job_owner), job_future)
                finally:
                    blocker.rollback()
                assert native.result(timeout=10)
                job_future.result(timeout=10)
        finally:
            blocker.rollback()
        assert api.get_repo(repo).head_cmmt_id != head and store.active_barrier(scopes[0])
        assert write(original, api.get_repo(repo).head_cmmt_id) is None
        claim = store.claim("probe-worker", kinds=("authorization.refresh",))
        store.complete(claim, barrier_guard=reconciliation_fixture)

        # Kill only this probe's identified native SQL transaction while it owns
        # authority scopes. No connection retry may publish after losing them.
        head = api.get_repo(repo).head_cmmt_id
        with blocker.cursor() as cursor:
            cursor.execute("SELECT gc_id FROM GCID WHERE repo_id=%s FOR UPDATE", (repo,))
        with ThreadPoolExecutor(max_workers=1) as pool:
            pending = pool.submit(write, original, head)
            try:
                owner = wait_sql(admin,
                    "SELECT t.PROCESSLIST_ID FROM performance_schema.data_lock_waits w "
                    "JOIN performance_schema.data_locks l ON l.ENGINE_LOCK_ID=w.REQUESTING_ENGINE_LOCK_ID "
                    "JOIN performance_schema.threads t ON t.THREAD_ID=l.THREAD_ID "
                    "WHERE l.OBJECT_SCHEMA=%s AND l.OBJECT_NAME='GCID' AND t.PROCESSLIST_USER=%s",
                    (database, native_user), pending)
                with admin.cursor() as cursor:
                    cursor.execute("KILL CONNECTION " + str(int(owner)))
            finally:
                blocker.rollback()
            assert pending.result(timeout=10) is None
        assert api.get_repo(repo).head_cmmt_id == head
        assert write(original, head)
    return {"barrier_missing_schema_rejected": True, "barrier_write_and_noop_rejected": True,
            "barrier_failed_cancelled_still_fenced": True, "barrier_and_publish_both_orders_serialized": True,
            "barrier_native_connection_loss_no_publish": True, "barrier_unicode_scope_parity": True,
            "barrier_final_readonly_race_rejected": True}


def run(server_binary, fileserver_binary, *, check_strict=False, check_barriers=False):
    import pymysql
    from pymysql.constants import CLIENT
    if os.environ.get("CF_TEST_NATIVE_COMMIT") != "1":
        raise ValueError("explicit isolated native probe opt-in is required")
    if not __debug__:
        raise ValueError("native evidence probe cannot run with assertions disabled")
    port = int(os.environ["CF_TEST_DB_PORT"])
    if not 1 <= port <= 65535 or not all(Path(item).is_absolute() and Path(item).is_file()
                                       for item in (server_binary, fileserver_binary)):
        raise ValueError("invalid isolated probe configuration")
    host = os.environ.get("CF_TEST_DB_HOST", "127.0.0.1")
    admin = pymysql.connect(host=host, port=port, user="root", password="", autocommit=True,
                            client_flag=CLIENT.MULTI_STATEMENTS, connect_timeout=5, read_timeout=10)
    token = uuid4().hex
    databases = []
    user = "cf_probe_" + token[:20]
    password = secrets.token_urlsafe(32)
    account_created = False
    processes = []
    try:
        with admin.cursor() as cursor:
            cursor.execute("CREATE USER %s@'%%' IDENTIFIED BY %s", (user, password))
            account_created = True
            for kind in ("ccnet", "seafile"):
                database = "cf_native_probe_" + token + "_" + kind
                cursor.execute("CREATE DATABASE " + database)
                databases.append(database)
                cursor.execute("GRANT ALL ON " + database + ".* TO %s@'%%'", (user,))
                cursor.execute("USE " + database)
                schema = Path(__file__).resolve().parents[1] / "scripts/sql/mysql" / (kind + ".sql")
                cursor.execute(schema.read_text())
                while cursor.nextset():
                    pass
        with tempfile.TemporaryDirectory(prefix="cf_native_probe_") as temporary:
            root = Path(temporary)
            config, data, ccnet = (root / name for name in ("conf", "data", "ccnet"))
            for directory in (config, data, ccnet):
                directory.mkdir()
            with socket.socket() as reservation:
                reservation.bind(("127.0.0.1", 0))
                http_port = reservation.getsockname()[1]
            (config / "seafile.conf").write_text(
                "[database]\ntype=mysql\nhost=" + host + "\nport=" + str(port) + "\nuser=" + user
                + "\npassword=" + password + "\ndb_name=" + databases[1]
                + "\n[fileserver]\nuse_go_fileserver=true\nhost=127.0.0.1\nport=" + str(http_port) + "\n")
            (config / "seafile.conf").chmod(0o600)
            environment = {**os.environ, "SEAFILE_MYSQL_DB_CCNET_DB_NAME": databases[0],
                           "SEAFILE_MYSQL_DB_SEAFILE_DB_NAME": databases[1], "JWT_PRIVATE_KEY": secrets.token_urlsafe(32),
                           "SEAFILE_MYSQL_DB_HOST": host, "SEAFILE_MYSQL_DB_PORT": str(port),
                           "SEAFILE_MYSQL_DB_USER": user, "SEAFILE_MYSQL_DB_PASSWORD": password,
                           "SEAFILE_CENTRAL_CONF_DIR": str(config), "SEAFILE_CONF_DIR": str(data)}
            os.environ.update({key: environment[key] for key in (
                "SEAFILE_CENTRAL_CONF_DIR", "SEAFILE_CONF_DIR")})
            # Stop children before removing their temporary data and log files.
            with (root / "process.log").open("wb") as log, ExitStack() as cleanup:
                server = subprocess.Popen([server_binary, "-F", str(config), "-c", str(ccnet), "-d", str(data),
                                           "-l", str(root / "server.log"), "-f"], env=environment,
                                          stdout=log, stderr=log)
                processes.append(server)
                cleanup.callback(stop_process, server)
                from seaserv import seafile_api as api, ccnet_api
                wait_ready(lambda: api.get_repo_list(0, 1), server)
                actor = "native-probe@example.invalid"
                ccnet_api.add_emailuser(actor, secrets.token_urlsafe(32), False, True)
                repo = api.create_repo("native probe", "isolated diagnostic", actor)
                original = root / "original"
                changed = root / "changed"
                stale = root / "stale"
                original.write_bytes(b"original")
                changed.write_bytes(b"changed")
                stale.write_bytes(b"stale")
                api.post_file(repo, str(original), "/", "probe.txt", actor)
                base = api.get_repo(repo).head_cmmt_id
                original_id = api.get_file_id_by_path(repo, "/probe.txt")
                api.put_file(repo, str(changed), "/", "probe.txt", actor, base)
                current = api.get_repo(repo).head_cmmt_id
                returned = api.put_file(repo, str(original), "/", "probe.txt", actor, base)
                results = {"c_same_hash_stale_noop": returned == original_id and current != base
                           and api.get_repo(repo).head_cmmt_id == current
                           and api.get_file_id_by_path(repo, "/probe.txt") != returned}
                api.put_file(repo, str(stale), "/", "probe.txt", actor, base)
                results["c_stale_base_write_accepted"] = api.get_repo(repo).head_cmmt_id != current

                # Separate RPC clients synchronize at the call boundary; this is
                # real concurrent entry execution, not a simulated branch update.
                from seafile import ServerThreadedRpcClient
                barrier = threading.Barrier(2)
                concurrent_base = api.get_repo(repo).head_cmmt_id
                def write(index):
                    path = root / ("concurrent-" + str(index))
                    path.write_bytes(("writer-" + str(index)).encode())
                    client = ServerThreadedRpcClient(str(data / "seafile.sock"))
                    barrier.wait(timeout=5)
                    return client.put_file(repo, str(path), "/", "probe.txt", actor, concurrent_base)
                with ThreadPoolExecutor(max_workers=2) as pool:
                    responses = list(pool.map(write, (1, 2)))
                results["c_concurrent_accepted_count"] = sum(isinstance(value, str) for value in responses)
                if check_strict:
                    results.update(check_strict_primitive(api, repo, actor, root, data, original, changed, stale,
                                                         admin, databases[1], user))
                if check_barriers:
                    results.update(check_barrier_primitive(api, repo, actor, data, original, changed,
                                                          admin, databases[1], user))

                fileserver = subprocess.Popen([fileserver_binary, "-F", str(config), "-d", str(data),
                                               "-l", str(root / "fileserver.log")], env=environment,
                                              stdout=log, stderr=log)
                processes.append(fileserver)
                cleanup.callback(stop_process, fileserver)
                http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                def ready_http():
                    with http.open("http://127.0.0.1:" + str(http_port) + "/protocol-version", timeout=2) as response:
                        return response.status
                wait_ready(ready_http, fileserver)
                go_base = api.get_repo(repo).head_cmmt_id
                api.put_file(repo, str(changed), "/", "probe.txt", actor, go_base)
                before_go = api.get_repo(repo).head_cmmt_id
                web_token = api.get_fileserver_access_token(repo, '{"parent_dir":"/"}', "update", actor, False)
                results["go_stale_base_http_status"], _ = web_update(http, http_port, web_token, go_base, b"go-stale-write")
                results["go_stale_base_write_accepted"] = before_go != go_base and api.get_repo(repo).head_cmmt_id != before_go
                api.put_file(repo, str(original), "/", "probe.txt", actor, api.get_repo(repo).head_cmmt_id)
                noop_base = api.get_repo(repo).head_cmmt_id
                noop_id = api.get_file_id_by_path(repo, "/probe.txt")
                api.put_file(repo, str(changed), "/", "probe.txt", actor, noop_base)
                noop_current = api.get_repo(repo).head_cmmt_id
                status, returned = web_update(http, http_port, web_token, noop_base, b"original")
                results["go_same_hash_stale_noop"] = status == 200 and returned == noop_id and noop_current != noop_base \
                    and api.get_repo(repo).head_cmmt_id == noop_current \
                    and api.get_file_id_by_path(repo, "/probe.txt") != returned
                return results
    finally:
        for process in reversed(processes):
            stop_process(process)
        with admin.cursor() as cursor:
            for database in databases:
                cursor.execute("DROP DATABASE " + database)
            if account_created:
                cursor.execute("DROP USER %s@'%%'", (user,))
        admin.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server-binary", required=True)
    parser.add_argument("--fileserver-binary", required=True)
    parser.add_argument("--check-strict", action="store_true")
    parser.add_argument("--check-barriers", action="store_true")
    args = parser.parse_args()
    print(json.dumps(run(args.server_binary, args.fileserver_binary, check_strict=args.check_strict,
                         check_barriers=args.check_barriers), sort_keys=True))
