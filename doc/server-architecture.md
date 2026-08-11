<!-- generated-by: gsd-doc-writer -->
# CloudFile Server 边界、扩展点与证据

> 用途：说明本仓在 CloudFile 三仓架构中的终判职责，并标注各扩展的实现和验证状态。
> 适用版本：CloudFile `dev`（`fb16a0b`），基于本地 Seafile CE 14 上游快照 `0e83ffd`。
> 状态：验证中；代码已合入，文件锁和真实 S3 环境仍有验证缺口。

## 系统边界

本仓复用 Seafile CE 的 library、commit/fs/block 对象模型、seaf-server、Go fileserver
和 RPC 基础设施，只承载绕开 Seahub 仍必须生效的强制规则。UI、OnlyOffice 会话编排、
SSO 同步、搜索任务、SMB/NFS 连接器和部署编排属于 Hub 或 Docker 仓；本仓最多提供
底层 RPC、对象存储实现和共享 `cf_*` schema。

```text
桌面同步 / WebDAV / Seahub
          │
          ├── Go fileserver ── cf_ext.go / cf_fileop.go ──┐
          │                                                │ RPC
          └── seaf-server 写入口 ─ repo-op.c ──────────────┤
                                                           ▼
                 cf-ext（读侧） / cf-fileop（写侧）
                       │                 │
                       ├── 目录 ACL      └── 租约文件锁
                       │
                       └── seafile-db 的 cf_* 表

对象读写 ── obj/block/fs manager ── filesystem / S3 / multiple backend
```

## CE 复用、本项目新增与外部组件

| 边界 | 复用 Seafile CE | CloudFile 新增 | 外部依赖或归属 |
|---|---|---|---|
| 权限 | 原生库级权限与共享模型 | `cf-ext` 链式收紧、目录 ACL、同步子树检查 | ACL 规则管理 UI/API 在 Hub |
| 写入 | `seaf_repo_manager_*` 与 fileserver 写入口 | PREPARE / COMMITTED / ABORTED 生命周期、文件锁 provider | 生命周期共享用例在 Docker 仓 |
| 存储 | commit/fs/block 抽象和 filesystem backend | S3 backend、多存储路由、迁移工具 | S3/MinIO 服务和部署配置在 Docker 仓 |
| 数据 | seafile-db 与现有 ccnet 数据 | 独立 `scripts/sql/*/cloudfile.sql` | bootstrap 应用 schema 的逻辑在 Docker 仓 |
| 应用扩展 | RPC 和 Python 客户端 | lock RPC、限制路径 RPC、schema 占位 | SSO、搜索、外部源、Local 会话逻辑主要在 Hub |

## 扩展点和失效语义

读侧入口位于 `common/cf-ext.c`。`cf_ext_init()` 会调用 ACL、测试 provider 和文件锁
初始化，但它们只在各自开关打开时注册；全部关闭时 provider 表为空，原生 CE 权限
原样返回。权限 provider 按注册顺序执行，只允许收紧结果；ACL 数据读取失败时拒绝
访问。

写侧入口位于 `common/cf-fileop.c`，C 与 Go 写入口都发送统一事实。没有 provider
时写入透传；一旦确认存在 provider，Go fileserver 无法联系 seaf-server 时拒绝写入，
避免锁或其他终判能力被瞬时 RPC 故障绕过。测试 provider 只是门禁仪器，默认关闭，
不得作为生产能力启用。

## 能力状态

| 能力 | 产品定位 | 状态 | 代码证据 | 测试证据 |
|---|---|---|---|---|
| 读侧扩展注册表 | CE 补强 | 已完成 | `common/cf-ext.{c,h}`、`common/rpc-service.c` | ACL 规则测试；完整 C 构建由 CI 执行 |
| 目录 ACL 终判 | Pro 平替 | 已完成 | `common/cf-acl*.{c,h}`、`fileserver/cf_ext.go` | `tests/cf-acl/run.sh` 与共享用例 |
| 写入生命周期 | CE 补强 | 已完成 | `common/cf-fileop*`、`server/repo-op.c`、`fileserver/cf_fileop.go` | `tests/cf-fileop/run.sh`、Go `Cf*` 契约测试 |
| 租约文件锁 | Pro 平替 | 验证中 | `common/cf-lock.{c,h}`、5 个 `cf_lock_*` RPC、lock 表 | 尚无本仓专用锁集成测试；依赖完整 C 构建 |
| S3 对象后端 | Pro 平替 | 部分完成 | C/Go S3 backend 与 `cf-s3-client` | Go 配置测试已存在；C 测试需真实端点 |
| 多存储与迁移 | Pro 平替 | 部分完成 | `storage-backend-multi.*`、`backend_multi.go`、`seaf-storage-migrate.c` | Go 路由测试已存在；迁移回滚需部署级验证 |
| SSO / 搜索状态 | 新应用扩展 | 部分完成 | `cf_sso_*`、`cf_search_index_state` 表 | 本仓只提供 schema；行为由 Hub/Docker 验证 |
| 外部源与 overlay | 新应用扩展 | 部分完成 | `cf_external_*` 表 | 本仓不读取这些表；连接器与路径约束在 Hub |
| Local 编辑会话 | 新应用扩展 | 规划 | `cf_edit_session` 表 | 本仓暂无运行时代码 |

“已完成”表示实现、仓内门禁和 `dev` 合入均有证据，不代表生产部署已验收；部署验收
统一记录在 Docker 仓。

## 配置和 schema 边界

本仓直接读取的 `[cloudfile]` 键只有：

| 配置 | 默认 | 作用 |
|---|---|---|
| `dir_acl_enabled` | 关闭 | 注册目录 ACL provider |
| `file_lock_enabled` | 关闭 | 请求启用租约文件锁 |
| `lock_backend` | 未设置，等同 `cloudfile` | CE 构建拒绝非 `cloudfile` backend |
| `fileop_test_provider_enabled` | 关闭 | 启用测试 provider；禁止生产使用 |
| `fileop_test_refuse_token` | 未设置 | 测试拒绝标记 |
| `fileop_test_journal` | 未设置 | 测试事实日志路径 |

存储类由 `[storage] enable_storage_classes`、`storage_classes_file` 或
`CF_STORAGE_CLASSES_JSON` 控制；S3 backend 的完整部署示例由 Docker 仓维护。
`scripts/sql/mysql/cloudfile.sql` 与 `scripts/sql/sqlite/cloudfile.sql` 是本仓 schema
真相来源，当前共 11 张表，分属 ACL、SSO 状态、搜索状态、外部源、租约锁和编辑会话。

## 上游改动面

相对本地 `upstream/master`（`0e83ffd`，2026-07-26），当前分支新增 42 个文件、修改
36 个上游文件。新增 `cf-*` / `cf_*` 文件冲突风险较低；主要同步风险集中在 RPC、
repo 写入口、Go fileserver、对象存储接口和 GC。每次同步后用以下命令重算，不维护
容易失真的手写全量表：

```bash
git diff --name-status upstream/master...HEAD
git diff --check upstream/master...HEAD
```

## 上游 PR 评估

| 候选 | 评估 | 状态 | 理由 |
|---|---|---|---|
| `e398c5e`：从 `python/seaserv` 包根导出 repo 状态常量 | 建议单独提交 | 待确认 | 单文件兼容性修复，不依赖 CloudFile schema 或扩展点 |
| `f126e6e`：拒绝无效对象存储配置及对应测试 | 可拆分后提议 | 待确认 | 错误处理通用，但当前测试依赖 CloudFile S3 backend，需先缩小补丁 |
| S3 backend、storage classes、迁移工具 | 暂不直接提交 | 部分完成 | 范围大且涉及 CE/Pro 产品边界，应先确认上游接受策略再拆分 |
| ACL、文件锁、写生命周期、`cf_*` RPC | 不建议 | 已完成 | CloudFile 产品语义和跨仓契约强耦合，不是独立上游修复 |
| `cloudfile.sql` 中的应用扩展表 | 不建议 | 部分完成 | 由 CloudFile bootstrap 与 Hub 消费，上游没有对应运行时契约 |

任何候选 PR 都应从最新上游新建最小分支，补上独立测试，不携带 `cf_*` schema、
跨仓链接或产品开关。

## 验证入口

测试命令、CI job 和本地限制见 [README.testing.md](../README.testing.md)。Git 证据以
`dev` 提交和本地 `upstream/master` 为准；上游远端若已推进，应先更新快照再复核
PR 评估和 36 个修改文件的统计。
