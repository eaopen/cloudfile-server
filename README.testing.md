<!-- generated-by: gsd-doc-writer -->
# CloudFile Server 测试指南

> 用途：给 server 维护者提供可复现的本地门禁、完整 CI 路径和已知验证缺口。
> 适用版本：CloudFile `dev`，面向 Seafile CE 14 扩展分支。
> 状态：已完成；真实 S3 与完整 C 构建需 Linux/CI 环境。

## 快速门禁

需要 Go 1.22、Python 3、C 编译器、`pkg-config` 和 glib 开发包。ACL 与 fileop
用例默认从相邻 `cloudfile-docker/docs/` 读取共享 JSON；不使用并排 checkout 时，
分别设置 `CF_ACL_CASES` 和 `CF_FILEOP_CASES`。

```bash
./tests/cf-acl/run.sh
./tests/cf-fileop/run.sh
# 目录分页另需 valac / libjansson 开发包
./tests/cf-dir-page/run.sh

cd fileserver
go build ./...
go vet ./...
go test -count=1 -run 'Cf[A-Z]' .
```

`go test ./...` 不是 CloudFile 秒级门禁：上游 `repomgr` 测试依赖 MySQL。需要完整
上游测试时使用 CI 环境，不要把这类环境失败误判成 CloudFile 契约回归。

## 各测试覆盖什么

| 命令 | 覆盖 | 不覆盖 |
|---|---|---|
| `tests/cf-acl/run.sh` | ACL 解析、继承、优先级、权限不放宽不变量 | 数据库查询、RPC 和进程集成 |
| `tests/cf-dir-page/run.sh` | 生产 C 扫描、RPC、ACL 过滤及相邻 Hub 端点的共享分页用例 | 存储/数据库与进程间 RPC 使用 fixture，不等于部署 E2E |
| `tests/cf-fileop/run.sh` | 路径/操作词汇、provider 分发、空 provider 透传、C 写入口字段与参数形状 | 运行时变量值是否传对、完整 seaf-server 编译 |
| Go `Cf*` 测试 | C/Go 词汇、JSON 字段、错误码、共享用例 | MySQL 依赖的上游测试 |
| `tests/cf-s3/run.sh` | C commit/fs/block S3 backend 与错误语义 | 未设置端点时会跳过，不构成 S3 验收 |
| `ci/run.py` | 上游构建、MySQL、C/Go fileserver 和 pytest | 需要 Linux 与完整依赖链 |

## S3 集成测试

只对隔离的测试 bucket 运行；测试会创建和删除对象。不要使用生产 bucket 或把真实
密钥写进仓库。

```bash
CF_S3_TEST_ENDPOINT=http://127.0.0.1:9000 \
CF_S3_TEST_BUCKET=cf-s3-c-test \
CF_S3_TEST_KEY_ID=test-key \
CF_S3_TEST_SECRET_KEY=test-secret \
./tests/cf-s3/run.sh
```

只有 `CF_S3_TEST_ENDPOINT` 必须显式设置；其余变量在测试代码中有 MinIO 开发默认值。
设置 `CF_S3_TEST_PAGINATION=1` 会额外覆盖分页列举。没有 endpoint 时脚本返回成功并
打印 `SKIP`，汇报验证结果时必须把跳过单列出来。

## CI 门禁

| Workflow / job | 触发 | 实际入口 |
|---|---|---|
| `.github/workflows/cloudfile-checks.yml` / `checks` | `dev` push、PR、手工 | 相邻三仓布局后运行 `cloudfile-docker/tools/run-checks.sh` |
| `.github/workflows/cloudfile-checks.yml` / `build-c` | 同上 | `ci/install-deps.sh` 后运行 `ci/run.py`，真正编译完整 C 代码 |
| `.github/workflows/ci.yml` / `build` | push、PR | 上游 `ci/run.py` 路径 |
| `.github/workflows/golangci-lint.yml` | push、PR | fileserver 与 notification-server 的 golangci-lint |

`build-c` 把本仓 checkout 为 `seafile-server`，因为 `ci/run.py` 依赖这个目录名。
macOS 本地只能可靠运行纯 glib 测试与 Go 门禁；修改了 C 集成代码而未跑 Linux 构建
时，状态应写“验证中”，不能写“已验证”。

## 上游测试入口

`run_tests.sh` 只是 `ci/run.py --test-only` 的包装。它假设依赖和二进制已经安装；
当前推荐由 CI 先执行 `ci/install-deps.sh` 和完整 `ci/run.py`。2018 年的手工安装说明
已移到 [doc/history/README.testing.2018.md](doc/history/README.testing.2018.md)。

## 遗留并发上传工具

`tests/test_upload/` 是手工压力工具，不做断言、不清理上传结果，也不在 workflow 中
运行。它的当前限制和安全要求见 [tests/test_upload/readme.md](tests/test_upload/readme.md)。

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
The internal context path now uses CE read/write as qualification and loads
persisted cf_dir_acl rules from the same transaction into the shared C core.
Directory rw may elevate CE read; file none/invisible and hidden ancestors deny.
Native suspension/hard-readonly still reject before policy. The target is derived
from normalized RPC parent/file parameters, not supplied as a separate grant.
Snapshot departments include inline ancestors, roles use exact provider/namespace
identities; user namespace is fixed to `user`, external ID remains business userId.
Candidate lookup pins metadata, checks full indexes/InnoDB, then locks at most
4096 indexed ancestor rules; no library-wide rule scan. Context is checked again
after policy reads so expiry during waits rejects. This is still one internal
replacement primitive, not protection of ordinary CE/Go/WebDAV or complete
lifecycle/lock/facts enforcement. Development is unbuilt/unverified.
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
