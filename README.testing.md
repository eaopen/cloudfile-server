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
