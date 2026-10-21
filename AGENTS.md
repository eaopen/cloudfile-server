# AGENTS.md — cloudfile-server

> **当前规则**：本仓 GitHub 默认分支 `dev` 就是 **CloudFile V2** 开发主线，基于 Seafile CE14。不要把 `dev` 当作 V0.x、`dev2`、`dev2-clean` 或纯上游 CE 快照。本文件约束 AI 开发 Agent 与维护者。

## 分支：简单、统一

CloudFile Server、Hub、Docker 共用以下规则：

| 分支 | 用途 |
| --- | --- |
| `dev` | 唯一当前开发主线（V2） |
| `feat/<功能>` | 新功能，从 `dev` 创建 |
| `fix/<问题>` | 缺陷修复，从 `dev` 创建 |
| `docs/<主题>` | 文档调整，从 `dev` 创建 |
| `archive/<历史名称>` | 历史快照，不作为新开发基线 |

**不要**再为新任务增加 `v2/`、版本号、日期、任务 ID 等必填层级，也不要建立新的 `dev2`/`dev3`。旧在途分支允许完成原任务，不强制改名。V0.x、原 `dev2` 与 `dev2-clean` 仅用于历史追溯；从旧版移植代码须单独审查，不得整枝合入 `dev`。

跨仓任务可使用相同的简短分支名，但只在确有代码改动的仓库创建分支。**同名分支不代表兼容**；联动构建、联验或发布须记录 Server、Hub、Docker 的准确 Commit SHA。分支权威说明见 [Docker BRANCHING.md](https://github.com/eaopen/cloudfile-docker/blob/dev/BRANCHING.md)。

## 本仓职责

本仓是 `haiwen/seafile-server` 的长期跟随型 fork，主要负责不可绕过的底层职责：文件对象与版本、C/Go 文件访问、存储路由、目录 ACL 的最终执行、提交/Checkout Guard 和对 Hub 的内部 RPC。Web UI、搜索任务、企业业务配置及前端授权展示不应搬进 Server。

优先复用 Seafile CE 原生能力；新增代码尽可能放在独立 `cf-*` / `cf_*` 文件。确需修改原生调用点时，先说明绕过风险和不可替代理由，在 [Docker 上游改动清单](https://github.com/eaopen/cloudfile-docker/tree/dev/docs/upstream-patches) 登记真实变更，不因为一项功能就扩大无关的上游修改面。

- 授权读取失败应拒绝，不能从受保护的 V2 入口回退到宽松的旧版实现。
- 原生 Repo、Block、FS、Commit 以及 CE 用户/组/权限事实尽量复用；不重建 V0.x 已废弃的管理表和功能开关。
- V2 内置目录 ACL 等功能按照实际代码与 Schema 生效，不再恢复 `v2_acl_enabled` 之类废弃开关。
- 不擅自改变正在运行的实例、数据库、对象存储或历史库迁移方案；额外改进先记录，未经确认不扩范围。

## 效率与验证

**不要求每次修改都编译、跑全部测试或构建镜像。** 纯文档修改无需编译或测试；小范围代码改动只做真正受影响的检查并复用已有可信证据。C/Go 全量编译、跨仓真实运行和镜像构建仅用于有必要的集成节点、核心运行行为变更或明确发布任务。未执行的验收须如实标注，不以“代码已提交”代替“DEV 已验证”。

编译/专项验证入口按受影响代码参考 [README.testing.md](README.testing.md)；三仓版本锁与部署由 Docker 管理，**不得直接采用旧 `release.yaml` 名称或旧分支推断当前 V2 镜像源码**。

## 代码习惯

代码和提交信息用英文，中文维护文档；C 保持上游风格，RPC 优先 `cf_` 前缀；明确内存所有权。不要提交构建产物、密钥、开发环境临时文件。

## 当前数据库（未正式发布 V2）

- 唯一扩展表为 `cf_library_storage`、`cf_dir_acl`、`cf_sso_group_map`、`cf_edit_guard`、`cf_audit_event`；新装一次创建全部五表，已有卷只校验。
- 不迁移或追溯开发期旧数据。存储绑定从 Seafile 的 Repo/RepoStorageId 重新识别，ACL/共享和组织/角色从 Etech 重新同步，Guard/审计从空表开始；旧 `cf_tag`、Resource、Intent、Session、Outbox 不恢复。
- 绝不清除 CE 原生 Repo、Branch、文件内容、版本或原生 Tag，也不因为缺表自动建立旧扩展 DDL、迁移服务或产品级升级框架。

## Agent2 本地文件编辑

- V2 本地查看/普通编辑/独占编辑经 Hub 签发 scoped JWT，Server Go `/cf-edit/v1/*` 复核权限、原生 File ID 与 Guard。
- 只保留 `cf_edit_guard` 的独占编辑状态；Seafile 管理原生文件版本，Agent2 本地 Journal 持有未确认任务。普通编辑不创建 Guard 或 Session。
- C 层历史编辑证明、Resource、Intent、Edit Session 通道已退出，调用应拒绝，不能为修复 `cf_edit_session` 缺表错误恢复旧 DDL 或旧分支 SQL。

## SQL 表边界

- 当前 Server V2 只允许 `cf_library_storage`、`cf_dir_acl`、`cf_sso_group_map`、`cf_edit_guard`、`cf_audit_event` 业务表。历史 `cf_library_storage_key` 的分配和冷 GC 账本不允许重新引入；存储依赖 `cf-v2-storage.c` 的预占与激活。
- `scripts/sql/mysql/cloudfile.sql` 和 `scripts/sql/sqlite/cloudfile.sql` 属于未发行旧设计，已从产品源码删除。旧数据库迁移属于外置一次性工具，不属于构建/启动路径。

## 现行 V2 权限契约：禁止“托管库”复活（2026-10-21）

- `dev` **不存在托管库/非托管库两种类别**。任何 `cf_dir_acl` 行只代表其目标路径和主体的显式规则，不能被用于判定整库被托管、整库访问关闭、旧入口一律拒绝或重新进入单独的库级安全模式。
- 唯一权限链：Seafile CE 原生库资格 + Hub `Authority.effective()` 对当前身份和实际路径的 ACL/共享目录结果 + C/Go 真实文件与最终 Commit 终判。无整库资格但有 `share_mode=true` 的用户只从共享目录根进入，不得开放库根、父级或兄弟目录。
- **坚决禁止**恢复 `cf_managed_library`、`cf_managed_library_share`、`cf_library_share_revision`、`checkLegacyLibrary`、`cf_policy_check_legacy_library`、`managed_library_guard`、`allow_legacy_managed_reads`，也禁止恢复用于断言“库有任意 ACL 行就应全库拒绝”的测试。
- 没有可信当前用户/路径的整库同步或无范围票据，允许因该**操作缺乏授权证明**而拒绝；这不是库分类。任何有路径证明的 Web/API/Agent/下载/上传必须按实际目标裁决，不能用无关目录的 ACL 拒绝整个库。策略异常不回退宽权限。
- 当前文档与测试应以 V2 权限终判契约及五张扩展表为准；历史文档只用于追溯，不得作为实施依据。跨仓变更必须维护防复活的源码扫描和正反权限用例。
