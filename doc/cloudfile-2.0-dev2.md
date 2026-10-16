# CloudFile 2.0 Server dev2 最小安全底座

> 2026-10-16 · `dev2` 开发快照，非发行、非真实运行验收。

本分支在现有 CE14 + 已验证的 CloudFile 文件写入/目录权限接缝上渐进演进。起点保留了成熟的 C/Go 路径权限及 Branch 最终提交检查，避免退回过旧 `dev14` 时无意绕过已部署所需的安全点。

## 第一批实现

- 在原生 C `cf_dir_acl` 规则加载与共享根资格查询两处校验：`share_mode=1` 必须依附非根目录正向 `r/rw`；数据库内若出现 `none/invisible + share_mode=1`，读侧拒绝而不是当作可共享的根。
- Hub `dev2` 的 API 输入校验同样强制这一规则。**Hub 检查与 Server 终判须同时存在**，不得以 UI/反代替代后者。
- 不删除现有 `cf_managed_library` 老权限栅栏和 0.9 迁移。2.0 新装精简 Schema、旧库安全导入及执行切换仍需要完成后才能删除旧机制，防止未完成切换时放宽权限。

## 尚未通过的验收

- `./tests/cf-acl/run.sh`、`./tests/cf-fileop/run.sh`、Go build/test、Linux C 编译、CloudFile CE14 新装与回滚；当前没有连接的授权开发设备，暂未运行这些测试。
- Etech DEV 的旁路 SSO、稳定 UID、库/共享根/目录 ACL、真实浏览器上传下载与 Branch 授权终判均待集成与现场验收。

下一轮按 M1/M2 实施新装最小表、稳定 UID/GroupUser TTL、跨仓业务合约和现实 Etech 流程，不将本提交视为全量 R1/R2 完成。
