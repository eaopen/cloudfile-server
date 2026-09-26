# Seafile Server Tests

## Isolated CloudFile Commit Diagnostic

`tests/probe_native_commit.py` starts newly built C Server and Go fileserver against
an explicitly isolated MySQL test container. It requires `CF_TEST_NATIVE_COMMIT=1`,
`CF_TEST_DB_PORT` and optionally `CF_TEST_DB_HOST`; the test admin is root with an
empty password in that disposable container only. Never point these variables at
a project database. Supply absolute `--server-binary` and `--fileserver-binary`
paths and the new Server's Python bindings/libsearpc on `PYTHONPATH`.

The probe creates random schemas, a random test account and temporary libraries,
then stops both processes before removing their data and drops only those schemas
and account. No existing database name, library ID or HTTP endpoint is accepted.
`pymysql` is required by the harness; it is not a new Server runtime dependency.

The JSON records whether baseline stale-head writes are accepted, including the
same-content early-return path and two concurrent C RPC writers. These are risk
diagnostics, **not** passing authorization, file-lock or conditional-edit tests.
Running them in a fresh process is required because native Python RPC configuration
is loaded at import time. Full original upstream tests remain separate.

Add `--check-strict` to validate the separate local RPC version primitive
`seafile_cloudfile_put_file_if_head`: current-head write/no-op, stale-head and
invalid conditions, read-only/virtual rejection, and exactly one concurrent winner.
It conservatively conditions on the **whole repository head**. It neither changes
legacy `seafile_put_file` nor provides CloudFile permission/lifecycle/barrier/lease
coordination, a public upload endpoint or an enabled edit capability.

## Run it locally

To run the tests, you need to install pytest first:

```sh
pip install -r ci/requirements.txt
```

Compile and install ccnet-server and seafile-server
```
cd ccnet-server
make
sudo make install

cd seafile-server
make
sudo make install
```

Then run the tests with
```sh
cd seafile-server
./run_tests.sh
```

By default the test script would try to start ccnet-server and seaf-server in `/usr/local/bin`, if you `make install` to another location, say `/opt/local`, run it like this:
```sh
SEAFILE_INSTALL_PREFIX=/opt/local ./run_tests.sh
```
