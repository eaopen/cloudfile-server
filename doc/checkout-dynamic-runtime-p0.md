# CloudFile Checkout / Protection 动态库能力 P0（2026-11-16）

运行期能力的唯一权威是 `cf_repo_feature(repo_id, feature)`。表行存在表示该库启用了 `checkout` 或 `protection`；不存在默认 OFF。

## 无重启启停

使用已授权 CloudFile 平台管理员会话调用已有：
`POST /api/v2.1/admin/cloudfile/repos/<repo-uuid>/features/`，body 例如 `{"checkout": true}`。
修改成功后，Hub、C Server 和 Go Fileserver 通过数据库立即读取新的库级配置，无需修改配置文件或重启。旧的 `checkout_enabled`、`checkout_repo_ids`、`protection_repo_ids` 不能覆盖运行期表值（历史配置仅用于受控迁移）。

- C FileOp 拦截器总是在原生服务启动时注册，但仅对当前选中的 Checkout 库查询 Guard。
- Go 启动时验证 `cf_edit_guard` 的结构；即使未配置任何 Checkout 库也完成就绪探测。Guard 不存在或不可用时，已选中库拒绝签出/受保护写入；普通库不查询 Guard 行。
- Hub 在发票据前查询当前功能及已安装的 Guard Schema；Checkout 不再依赖旧的全局开关。
- 最终 C/Go Branch(master) 提交采用现有事务锁与动态策略读，避免能力变更期间旧客户端绕过。Go Agent2 的旧续租请求也在该锁下重新验证能力。

## 关闭能力

- `checkout=false`：Hub 在同一 Branch(master) 事务内检查未到期的 `cf_edit_guard`；存在活动租约返回 409，且同批其他功能不发生部分更新。租约签入/释放或过期后再重试。
- `protection=false`：仍受既有保护约束，存在 `cf_file_protection` 记录时返回 409。
- 禁止对未同步虚拟库单独设置；源库能力保持最终权威。
- 数据库查询失败不视为 OFF：已选择保护的写入必须拒绝；未选择库不会扫描文件保护和 Guard 行。

## 本次验证边界

已执行本地 C FileOp Fixture（验证启动后无需再调用初始化即可开启），Go fileserver 单元测试、DEV Django 隔离进程的管理 API / 新旧编辑入口单测。真实 Windows Agent2、跨进程 HTTP 业务 E2E、生产部署仍需单独验收。服务器替换为新代码需要一次正常版本升级；**升级之后每次修改库能力无需重启**。
