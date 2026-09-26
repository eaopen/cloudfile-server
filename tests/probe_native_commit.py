"""Diagnostic CE commit probe against explicitly isolated MySQL and new binaries.

This demonstrates baseline behavior, not CloudFile commit protection. It creates
only random test schemas/accounts and temporary libraries; no existing DB names,
credentials, repository IDs or server endpoints are accepted.
"""

import argparse
from contextlib import ExitStack
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


def run(server_binary, fileserver_binary):
    import pymysql
    from pymysql.constants import CLIENT
    if os.environ.get("CF_TEST_NATIVE_COMMIT") != "1":
        raise ValueError("explicit isolated native probe opt-in is required")
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
    args = parser.parse_args()
    print(json.dumps(run(args.server_binary, args.fileserver_binary), sort_keys=True))
