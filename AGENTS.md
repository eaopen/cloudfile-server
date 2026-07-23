# AGENTS.md — cloudfile-server

给在本仓库工作的 AI coding agent。人类同样适用。

## 这是什么

`haiwen/seafile-server` 的 fork，CloudFile（Seafile CE 企业扩展版）的**权限终判层**。

CloudFile 由三个仓库组成，通常并排 checkout：

```
workspace/
├── cloudfile-server/   fork of haiwen/seafile-server —— 本仓库
├── cloudfile-hub/      fork of haiwen/seahub        —— Web/API 层
└── cloudfile-docker/   fork of haiwen/seafile-docker —— 构建、镜像、部署、规格文档
```

跨仓规格与部署说明在 `cloudfile-docker/`：
[BRANCHING.md](../cloudfile-docker/BRANCHING.md)、
[docs/acl-semantics.md](../cloudfile-docker/docs/acl-semantics.md)。

## 本仓库的职责边界

**只放必须在底层执行的东西。** 判断标准很简单：绕开 Seahub 还能不能生效？

放这里：
- 目录 ACL 的最终校验
- 文件锁、签出状态的强制校验
- 操作事件与审计事件的产生
- 存储扩展
- 给 Hub 用的内部 RPC

**不要**放这里：UI、项目业务流程、OnlyOffice 会话管理、SMB/NFS 连接器、
搜索索引任务。这些都在 `cloudfile-hub/cloudfile_ext/`。

理由：C 代码的迭代成本远高于 Python，而上面这些东西并不需要底层强制。

## 最重要的一条：不要改上游文件

本仓库是长期跟随上游的 fork。**每多改一个上游文件，以后每次同步上游都要多付一次代价。**

新代码尽量放进新文件（`common/cf-*.c`、`fileserver/cf_*.go`、
`scripts/sql/*/cloudfile.sql`）。

目前改动的上游文件：

| 文件 | 改了什么 |
|---|---|
| `common/rpc-service.c` | 实现 `check_permission_by_path`，新增 `cf_find_restricted_path` |
| `include/seafile-rpc.h` | 新 RPC 声明 |
| `server/seaf-server.c` | 新 RPC 注册 |
| `server/seafile-session.c` | 启动时 `cf_acl_init()` |
| `server/Makefile.am` | 新增源文件 |
| `fileserver/sync_api.go` | 同步前的子树校验（两处） |
| `python/seaserv/api.py` | `is_repo_syncable` / `is_dir_downloadable` |
| `python/seafile/rpcclient.py` | 新 RPC 客户端声明 |

改动这份清单时，同步更新 `cloudfile-docker/BRANCHING.md`——那是同步上游时的
检查依据，失真就会漏掉冲突点。

**建表刻意放在新文件** `scripts/sql/{mysql,sqlite}/cloudfile.sql`，没有动上游的
`seafile.sql`，所以这块永远不会冲突。别把新表加回 `seafile.sql`。

## ACL 代码结构

```
common/cf-acl-resolve.{c,h}   纯策略，只依赖 glib
common/cf-acl.{c,h}           配置、数据库、群组查询
fileserver/cf_acl.go          同步客户端网关，走 RPC 问 seaf-server
tests/cf-acl/                 用共享用例集驱动的测试
```

**这个拆分是有意的**：`cf-acl-resolve.c` 不依赖 seaf session、数据库或 searpc，
因此可以脱离整个 seafile 构建单独编译测试。往里面加 `#include "seafile-session.h"`
就毁掉了这个性质——需要 I/O 就加在 `cf-acl.c`。

`fileserver/cf_acl.go` **不实现第三份求解逻辑**，只通过 RPC 问 seaf-server。
同步协议交换的是 commit、fs 对象和 block，不带路径，能做的只有在同步开始前
判断"这个库里是否存在该用户不可读的内容"。少一份实现 = 少一处漂移。

## 测试

```bash
./tests/cf-acl/run.sh
```

只需要 glib，不需要完整构建。用例集来自 `cloudfile-docker/docs/acl-cases.json`，
三仓并排 checkout 时自动找到，否则用 `CF_ACL_CASES` 指定。
**同一份用例集也驱动 cloudfile-hub 的 Python 实现**，两边必须给出相同结果。

Go 部分：

```bash
cd fileserver && go build ./... && go vet ./...
```

完整构建需要 Linux + autotools + searpc + glib，见
`cloudfile-docker/build/cloudfile_14.0/cloudfile-build.sh`。
**在 macOS 上无法完整构建**——只能验证 `cf-acl-resolve.c` 和 Go 部分。
改了 C 代码而没能编译，请在汇报时明确说出来。

## 铁律

**1. 开关关闭 = 原生 CE 行为。**

`seafile.conf` 的 `[cloudfile] dir_acl_enabled` 未开启时，所有 CloudFile 代码
路径必须是透传。`cf_acl_apply()` 在关闭时就是一次 `g_strdup`。

**2. 扩展只能收紧权限，不能放宽。**

`tests/cf-acl/test-cf-acl.c::test_never_widens` 会穷举验证。

**3. 读不到规则时 fail closed。**

数据库查询失败时拒绝访问，而不是放行。理由：正好在"限制规则读不出来"的时候
放行，是最糟糕的失败模式。注意区分"没有规则"（放行）和"读不出规则"（拒绝）——
`load_repo_rules()` 用 `db_error` 出参区分这两者。

**4. 无法解释的数据不能当作允许。**

`load_rule_cb` 遇到无法识别的 `subject_type` 或 `permission` 时跳过并告警，
不猜测。

## 约定

- 代码、注释、commit message 用英文；文档（`*.md`）用中文。
- C 代码沿用上游风格：4 空格缩进，`snake_case`，`seaf_warning()` / `seaf_message()` 打日志。
  文件头保留 `/* -*- Mode: C; tab-width: 4; ... -*- */`。
- 内存所有权在函数注释里写清楚。返回 `char *` 的一律是新分配、调用方 `g_free`。
- 新 RPC 命名以 `cf_` 开头，避免和上游将来的函数撞名。
- 不要提交 `.codegraph/`、构建产物。
