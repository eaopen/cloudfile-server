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
It also holds the GCID row lock, observes the native transaction's actual wait
through MySQL 8 `performance_schema.data_lock_waits`, then advances GC. Both a
changed-content write and a same-content no-op must fail without changing the
published head/file. Missing lock-wait evidence fails the probe; elapsed time is
not accepted as evidence. The disposable MySQL admin needs performance_schema
visibility. This tests final GC comparison, not a real garbage-collector run.
It conservatively conditions on the **whole repository head**. It neither changes
legacy `seafile_put_file` nor provides CloudFile permission/lifecycle/barrier/lease
coordination, a public upload endpoint or an enabled edit capability.

Add `--check-barriers` with the new Hub source package on `PYTHONPATH` to exercise
`seafile_cloudfile_put_file_with_barriers` against actual Hub JobStore SQL effects.
Its sixth argument is bounded JSON containing `head_id` and `scopes`, optionally
with `context: {"provider": "directory", "userId": "...", "epoch": "32 lowercase hex"}`.
Context-bearing calls also require the exact provider scope. They check current
Redis ready generation after GC and Branch locking, before publication, using
the same SQL scopes as Hub refresh begin/publish. Configure `subject_redis_host`,
`subject_redis_port`, `subject_redis_prefix` (e.g. `cf:subjects:`) and optionally
`subject_redis_password` in `[cloudfile]`; there is no implicit Redis fallback.
Use a private trusted Redis transport; this adapter does not implement TLS or
Redis ACL username authentication. Connection and command timeouts are one second.
Only Redis DB 0 is currently supported. Prefix must match the Hub deployment.
The key digest is SHA256 of compact UTF-8 JSON `[provider,userId]`, without ASCII
escaping. Older non-ASCII subject keys become cache misses, not database migration.
With explicit isolated `CF_TEST_REDIS_HOST/PORT`, the probe verifies missing,
wrong-user/epoch, refreshing, expired, persistent and lease-held snapshots,
Unicode parity, current write/no-op and rejection after generation change.
Snapshot input is a fixture: not real IdP/directory or full ACL evidence.
Barrier-only two-field calls remain internal compatibility primitives, not
current-context protected calls. Ordinary CE/Go entry points are not covered.

Development increment (not yet built/tested): context-bearing publication also
loads CE qualification on the same transaction, with owner > personal share >
groups > inner-public precedence. Duplicate personal shares, unknown permission,
broken hierarchy, non-InnoDB tables and unsafe transaction isolation reject.
Group membership/structures are batch-loaded with a 4096-group/128-depth budget;
implicit department ancestors are checked against locked parent/path rows.
REPEATABLE-READ or SERIALIZABLE is required to protect absent personal shares and
membership ranges, including a concurrent restrictive insertion. Qualification
is followed by the Redis final check, not the other way around.
Until the ACL loader is connected this internal path requires CE write; this is
an incomplete staging guard, not the final v2 policy. The ACL integration must
use CE read/write only as library qualification and allow approved directory
rw elevation while retaining native suspension/hard-readonly constraints.
The extended probe source covers personal-read precedence, group write and
implicit ancestors, but execution is deferred until overall feature verification.
Scopes use the job scope contract; at least a user scope and this repository's
scope are mandatory. **Trusted runtime assembles scopes; they are not grants.**
Scope acquisition shares one five-second monotonic budget, rounded down to
remaining whole seconds; exhausted budget makes subsequent locks nonblocking.
The native probe holds both user and repo locks, releases user after observed
waiting and keeps repo held; publication must fail within the shared budget.
It also runs the Hub's internal flat-role provisioner on real CE-created Group
tables and checks C RPC read-back, empty members and idempotent retry. Its
management guard/audit hook are explicit fixtures, not production authorization.
Native department roots and children are also created with GroupStructure and
checked through C RPC parent IDs, empty members and idempotent retry.
The same-connection membership projector is checked through real C group reads:
managed additions/removals, preservation of a manual group and no-op retry.
Its generation assertion/audit policy are fixtures; this is not Redis readiness
or complete login/data-entry enforcement evidence.
The probe uses the real schema runner and checks Unicode key parity, missing
schema failure, failed/cancelled barriers, changed-content/no-op rejection, both
barrier/publication orderings, final read-only change and native connection loss.
Actual `metadata_locks`/`data_lock_waits` evidence is required before each race.
It only kills the identified SQL thread belonging to its random native account.
The final publication transaction also locks and checks the exact native
`EmailUser` account on the configured CCNET database before repository rows. The
databases must share MySQL connection settings, the account table must be InnoDB,
and the runtime SQL user needs SELECT permission on it. Missing, duplicate,
case-mismatched, inactive or malformed accounts and database failures reject
publication. No account or authorization table is added. The probe tests both
suspension/publication orders, including the real CE account-management RPC,
nontransactional tables and revoked database access. RPC integer flags use 0/1,
not JSON booleans.
The same final transaction then locks the exact Hub `profile_profile` binding:
all user scopes must carry one business userId, matching `login_id`, and `user`
must match the native username. Configure the actual Hub schema explicitly:

```ini
[cloudfile]
identity_database = seahub_db
```

There is no default or email-based fallback. Hub must share the transaction's
MySQL server and credentials; the runtime user needs SELECT permission and the
table must be InnoDB. Missing, duplicate, case-mismatched, unbound or unreadable
bindings reject changed-content and no-op requests. This does not create users
or prove OIDC subject, current Redis epoch, qualification or ACL. The probe uses
a minimal CE-shaped Profile SQL fixture, not a full Django deployment, and
observes actual SQL waits for both unbind/publication orderings, including no-op.
Barrier completion uses an explicitly test-only reconciliation proof;
context/ACL/lifecycle/lock reconciliation is not claimed. Run without
Python `-O`; disabling assertions is rejected before creating any test schemas.

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
