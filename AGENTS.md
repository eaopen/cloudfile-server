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

## 开发期数据库变更（2026-10-21）

- 新版当前只明确新增四张业务表：`cf_library_storage`、`cf_dir_acl`、`cf_sso_group_map`、`cf_edit_guard`。历史表仅作备份或迁移输入，不属于新版运行库及运行依赖；增加业务表须先确认，历史数据单独归档，不直接删除。
- 开发期建表、升级及数据迁移通过 Docker 仓 `database/` 下独立、人工执行的 SQL 交付，DEV 验证与 PROD 发布复用固定脚本及校验和。启动/重启仅校验，不自动执行扩展 DDL、旧迁移链或兼容补丁。
- 仅明确审核的成熟版本之间才考虑自动升级；须定义精确源/目标范围和数据/中断恢复验证。不得根据 dev 分支、表名存在或旧台账自动推断升级路径，不自动回滚。

## Agent2 本地文件编辑

- V2 本地查看/普通编辑/独占编辑经 Hub 签发 scoped JWT，Server Go `/cf-edit/v1/*` 复核权限、原生 File ID 与 Guard。
- 只保留 `cf_edit_guard` 的独占编辑状态；Seafile 管理原生文件版本，Agent2 本地 Journal 持有未确认任务。普通编辑不创建 Guard 或 Session。
- C 层历史编辑证明、Resource、Intent、Edit Session 通道已退出，调用应拒绝，不能为修复 `cf_edit_session` 缺表错误恢复旧 DDL 或旧分支 SQL。
