<!-- generated-by: gsd-doc-writer -->
# CloudFile Server 文档索引

> 用途：给维护者提供本仓当前有效文档、历史材料和证据入口。
> 适用版本：CloudFile `dev`，面向 Seafile CE 14 扩展分支。
> 状态：已完成。

## 当前文档

| 文档 | 状态 | 内容 |
|---|---|---|
| [AGENTS.md](../AGENTS.md) | 已完成 | 仓库边界、上游同步纪律和开发约束 |
| [server-architecture.md](server-architecture.md) | 验证中 | server 边界、扩展点、能力状态、证据和上游 PR 评估 |
| [README.testing.md](../README.testing.md) | 已完成 | 本地测试、CI 门禁与验证限制 |
| [tests/test_upload/readme.md](../tests/test_upload/readme.md) | 暂停 | 遗留并发上传手工工具；不属于自动化门禁 |
| [CLAUDE.md](../CLAUDE.md) | 已完成 | 将 Claude Code 引导到统一约束 |

跨仓产品规格、统一能力矩阵、认证、存储总章和部署说明由相邻
`cloudfile-docker` 仓维护；本索引不复制那些内容。

## 历史资料

历史文件只解释过去的做法，不作为当前命令或架构依据。入口见
[history/README.md](history/README.md)。

## 证据优先级

1. 当前代码、SQL、CI workflow 和可执行测试；
2. `dev` 上已经合并的 Git 提交；
3. 本地 `upstream/master` 快照；
4. 文档说明。文档与前三项冲突时，先修正文档并记录验证缺口。
