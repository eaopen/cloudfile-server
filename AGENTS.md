<!-- generated-by: gsd-doc-writer -->
# AGENTS.md — cloudfile-server

> 用途：约束本仓库中的代码、文档、测试与上游同步工作。
> 适用版本：CloudFile `dev`，面向 Seafile CE 14 扩展分支。
> 状态：已完成（能力状态与证据索引见 [doc/README.md](doc/README.md)）。

给在本仓库工作的 AI coding agent。人类同样适用。

## 这是什么

`haiwen/seafile-server` 的 fork，CloudFile（Seafile CE 企业扩展版）的**权限终判层**。

`dev` = **扩展基线 + 已验收能力**，全部开关默认关闭。开发中的能力在
`feature/<耦合簇>`，**验收后合回 `dev` 并删除分支**——
不长期分叉，理由见 `cloudfile-docker/docs/BRANCHES.md` 第一节。

CloudFile 由三个仓库组成，通常并排 checkout：

```
workspace/
├── cloudfile-server/   fork of haiwen/seafile-server —— 本仓库
├── cloudfile-hub/      fork of haiwen/seahub        —— Web/API 层
└── cloudfile-docker/   fork of haiwen/seafile-docker —— 构建、镜像、部署、规格文档
```

跨仓规格与部署说明在 `cloudfile-docker/`：
[BRANCHING.md](../cloudfile-docker/BRANCHING.md)、
[docs/BRANCHES.md](../cloudfile-docker/docs/BRANCHES.md)。

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

## 最重要的一条：尽量不要改上游文件

本仓库是长期跟随上游的 fork。**每多改一个上游文件，以后每次同步上游都要多付一次代价。**

新代码尽量放进新文件（`common/cf-*.c`、`fileserver/cf_*.go`、
`scripts/sql/*/cloudfile.sql`）。

当前差异已经不止最初 seam 的 10 个文件：ACL（含 V3 Pro 兼容语义与 V2 委托管理）、
写入生命周期、S3/多存储和迁移工具合入 `dev` 后，相对本地 `upstream/master`
快照（`8c47d5f`，2026-08-11）共有 42 个已跟踪上游文件被修改，新增 51 个文件。
静态清单会再次失真，检查时以这条命令为准：

```bash
git diff --name-status upstream/master...HEAD | awk '$1 == "M" {print $2}'
```

改动集中在四类位置：

- seam 与 RPC：`common/rpc-service.c`、`include/seafile-rpc.h`、
  `server/seaf-server.c`、`server/seafile-session.c`、Python RPC 客户端；
- 写入口：`server/repo-op.c`、`fileserver/fileop.go`、`fileserver/sync_api.go`；
- 存储接口：`common/{obj,block,fs}-*`、`fileserver/objstore/*` 与 GC 工具；
- 构建接线：`configure.ac`、`server/Makefile.am`、`server/gc/Makefile.am`、
  `fuse/Makefile.am`、Go module 文件。

新增能力仍应优先放在 `cf-*` / `cf_*` 新文件中；确需新增上游改动时，必须同时更新
`cloudfile-docker/BRANCHING.md` 的同步清单，并在评审中说明为什么现有 seam 不足。

后两个（`repo-op.c`、`fileop.go`）是写入生命周期扩展点带来的，理由写在
`cloudfile-docker/docs/fileop-lifecycle.md` 第五节：seam 不能放在已经登记过的
`rpc-service.c`，因为 `upload-file.c`、虚拟库合并和 `copy-mgr` 都直接调用
`seaf_repo_manager_*`，绕过 RPC 层——**终判点不能有绕行路**。

改动这份清单时，同步更新 `cloudfile-docker/BRANCHING.md`——那是同步上游时的
检查依据，失真就会漏掉冲突点。

能力需要建表时，**放进新文件** `scripts/sql/{mysql,sqlite}/cloudfile.sql`，
不要动上游的 `seafile.sql`——那样永远不会冲突。基线本身不建任何表；
docker 仓的 bootstrap 找不到该文件时会跳过并告警。

## 扩展点：cf-ext

```
common/cf-ext.{c,h}        读侧扩展点：配置读取 + 能力注册表 + 三个分发钩子
common/cf-fileop.{c,h}     写侧扩展点：PREPARE / COMMITTED / ABORTED
common/cf-fileop-json.{c,h} 上面那个的 JSON 线格式（jansson 只出现在这里）
common/cf-fileop-test.c    门禁用的假 provider，默认关闭，**不是能力**
common/cf-path.{c,h}       路径规范化与组件匹配，两个扩展点共用同一份
fileserver/cf_ext.go       同步客户端网关，走 RPC 问 seaf-server
fileserver/cf_fileop.go    Go 写入口网关，同样走 RPC 问 seaf-server
```

`cf_ext_init()` 当前调用 ACL、测试 provider 和文件锁的初始化函数；每个初始化函数
都先读取运行时开关，全部关闭时不会注册任何 provider，钩子仍是透传，行为与原生 CE
一致。`cf-fileop-test.c` 由 `[cloudfile]
fileop_test_provider_enabled` 门控，**默认关闭**，且刻意不进 `CF_ENABLE_*` 清单——
那份清单里的每一项都是运维可以合理打开的产品能力，而它是写入生命周期门禁用的
仪器，能拒绝写入、每次写入都追加文件，注册时会打一条明说"不要在生产里跑"的警告。

为什么用运行时开关而不是编译期剔除：编译期剔除意味着门禁跑的镜像不是发出去的
那个。这个项目为此付过一次代价——一份手写的 `seahub_settings.py` fixture 通过了
测试，而真正生成的文件抛 `NameError`、把整个文件的 CloudFile 配置一起丢掉，
服务看起来还正常起来了。**测发出去的那个。**

`cf-fileop.c`、`cf-path.c` 刻意只依赖 glib，因此 `tests/cf-fileop/run.sh` 不需要
完整的 seafile 构建就能跑——与 `cf-acl-resolve.c` 同一条理由。规格见
`cloudfile-docker/docs/fileop-lifecycle.md`。

**为什么用注册表而不是直接调用某个能力：**

如果每个能力都要自己去改 `rpc-service.c`、`seaf-server.c`、
`seafile-session.c`，那么它们会在同样的行上互相冲突——开发期分支之间冲突，
合进 `dev` 之后彼此的改动纠缠在一起，每次同步上游都要重新分辨谁改了什么。

所以**基线把这些上游文件一次性改好**，全部调进 `cf-ext.c`。一个能力只需要：

- 新增 `common/cf-<能力>.c`（新文件，零成本）
- 在 `cf_ext_init()` 里加一行注册（`cf-ext.c` 是 CloudFile 自己的文件，零成本）
- 在 `server/Makefile.am` 加一行（**已在登记清单里，边际成本为零**）

使用现有权限 seam 的能力分支不应新增上游改动文件。如果发现权限终判缺少钩子，
先把通用钩子加进 `cf-ext.h`，不要让单个能力直接散改上游调用点。存储接口这类
现有 seam 无法覆盖的改造，按前述规则登记修改面和同步成本。

这条约束正是让 ACL 能够长期独立演进的原因：基线怎么同步上游，都不会增加
特性分支的负担。

### 能力实现的组织方式

能力自身建议拆成纯策略 + I/O 两半，例如 ACL 的
`cf-acl-resolve.c`（只依赖 glib）与 `cf-acl.c`（配置、数据库、群组查询）。
前者可以脱离整个 seafile 构建单独编译测试——往里面加
`#include "seafile-session.h"` 就毁掉了这个性质。

## 测试

`dev` 当前包含三组 CloudFile C 侧测试：目录 ACL、写入生命周期 seam 和 S3 集成。
ACL 与 fileop 只依赖 glib；S3 测试需要显式提供测试端点。完整命令、CI 边界和当前
验证结果见 [README.testing.md](README.testing.md)。

```bash
./tests/cf-acl/run.sh
./tests/cf-fileop/run.sh
CF_S3_TEST_ENDPOINT=http://127.0.0.1:9000 ./tests/cf-s3/run.sh
```

它还顺带做一件本机做不到的事的近似：`check-call-sites.py` 把 `repo-op.c` 里每个
`CF_FILEOP_*` 调用抽出来、把值换成对应类型的哑变量、再拿真正的 `cf-fileop.h`
编译一遍。`repo-op.c` 在 macOS 上编译不了，而拼错字段名、写错 operation、
少个逗号这类错误本来要等 CI 二十分钟才暴露。它**不**检查传的变量对不对——
`.name = parent_dir` 类型是对的，值是错的，那只能靠 review 和 E2E。

Go 部分的秒级门禁：

```bash
cd fileserver && go build ./... && go vet ./...
go test -count=1 -run 'Cf[A-Z]' .
```

完整构建需要 Linux + autotools + searpc + glib，见
`cloudfile-docker/build/cloudfile_14.0/cloudfile-build.sh`。
**在 macOS 上无法完整构建**——本地只能验证 Go 部分和不依赖 seafile 的纯策略
文件。真正的 C 编译由 CI 的 `build-c` job 完成（它把本仓库 checkout 成
`seafile-server` 以满足上游 `ci/run.py` 的目录名假设）。改了 C 代码而没能编译，
请在汇报时明确说出来。

## 铁律

**1. 没有能力注册 = 原生 CE 行为。**

`cf_ext_init()` 会调用能力初始化函数，但能力受 `seafile.conf` 里 `[cloudfile]`
的开关控制；全部关闭时没有 provider 注册，每个钩子都是透传。

**2. 扩展只能收紧权限，不能放宽。**

`cf_ext_check_permission` 把各能力串起来，每个都拿到上一个的结果。任何能力
返回的权限都不得高于入参，能力自身要有穷举验证这条不变量的测试。

**3. 读不到规则时 fail closed。**

能力的数据读取失败时应拒绝访问，而不是放行——正好在"限制读不出来"的时候放行，
是最糟糕的失败模式。注意区分"没有规则"（放行）和"读不出规则"（拒绝）。

**4. 无法解释的数据不能当作允许。**

遇到无法识别的枚举值时跳过并告警，不猜测。

## 约定

- 代码、注释、commit message 用英文；文档（`*.md`）用中文。
- C 代码沿用上游风格：4 空格缩进，`snake_case`，`seaf_warning()` / `seaf_message()` 打日志。
  文件头保留 `/* -*- Mode: C; tab-width: 4; ... -*- */`。
- 内存所有权在函数注释里写清楚。返回 `char *` 的一律是新分配、调用方 `g_free`。
- 新 RPC 命名以 `cf_` 开头，避免和上游将来的函数撞名。
- 不要提交 `.codegraph/`、构建产物。
