# CloudFile Server 当前架构（V2 / dev）

> 当前权威：2026-10-21。本文件只描述 CloudFile V2 / Seafile CE14 当前开发主线；历史 V0.x/V3 文档、托管库状态、旧 Session/Resource/Intent、独立库共享账本均不能作为代码与测试依据。构建与 DEV 运行验收单独记录。

## 基础职责

- **Seafile CE 原生**：Repo、RepoOwner、SharedRepo、RepoGroup、GroupUser、VirtualRepo、Commit、Branch、原生 Tag、Block/FS 与文件历史。保留 CE 用户、库及文件操作的既有数据事实。
- **Server C**：原生文件/目录权限适配、用户与路径资格验证、目录共享独立授权、关键写入最终提交/CAS、跨库路径与对象事务保护。无法证明具体操作范围时仅拒绝该操作。
- **Go FileServer**：文件字节传输、Range、分块上传/提交、Scoped 下载/本地编辑与独占 Guard；CloudFile V2 要求启用 Go FileServer。上传提交必须验证 token 绑定的父目录及目标文件，不得因其他目录存在 ACL 而整库拒绝。
- **Hub Authority**：统一计算当前用户、组织/角色、原生库资格、目录继承与 `share_mode` 共享根的有效权限。Server 通过内部鉴权接口消费同一裁决，不另造托管库分类器。

## 五张扩展表（唯一现行契约）

| 表 | 职责 |
| --- | --- |
| `cf_library_storage` | 原生库与可选存储的绑定 |
| `cf_dir_acl` | 当前目录路径/主体的 ACL 和共享根，默认继承、允许局部缩权与扩权 |
| `cf_sso_group_map` | 外部组织/角色与 CE GroupUser 的映射 |
| `cf_edit_guard` | 独占编辑 Guard |
| `cf_audit_event` | 新产生的审计事件 |

不需要 `cf_dir_admin`、`cf_managed_library`、`cf_managed_library_share`、`cf_library_share_revision`、旧 Resource/Session/Intent 等扩展表。无任何第五张表以外的迁移依赖；开发期旧数据不要求历史迁移。

## 权限原则

1. **没有托管库**：`cf_dir_acl` 存在一行不是整个 Repo 切换安全模式的信号。无关目录的 ACL 不阻断库根、兄弟目录及已授权文件。
2. **访问范围由真实用户+目标路径决定**：原生库 `r/rw` 与共享目录的独立资格在统一 Hub Authority 结合；无库权限但有共享根只能看到共享子树。
3. **实际文件操作终判**：Go/C 的读、写、分块上传及 Branch 发布不可绕过当前对象与路径检查。结构写入、无路径原生同步、对象型 Token 在无可验证 scope 时拒绝该操作，而非将库标记为托管。
4. **失败拒绝**：ACL SQL、Hub、身份/主体不可用不得回退更宽的 CE 权限；可区分的后端故障向调用者返回 unavailable。
5. **非 Go HTTP 服务器不能代替 V2 文件数据面**：必须启用 `ENABLE_GO_FILESERVER=true`；历史 C 同步/块路径仍需按是否可证明实际范围进行限制。

## 主要实现

- `common/cf-acl.c`、`common/cf-v2-acl.c`、`common/cf-v2-share.c`：目录/原生权限、公开链接目标范围及共享根资格。
- `fileserver/cf_v2_acl.go`、`fileserver/fileop.go`：基于真实用户和文件路径的 preflight 与最终提交。
- `fileserver/cf_acl_public_scope.go`：已验证链接的路径/子树 ACL 检查。
- `server/cloudfile-policy.c`：本地 C scoped/操作策略；不包含旧库状态登记。
- `tests/test_v2_no_managed_library.py`：阻止旧托管库表、函数、错误前置权限判断回流。

测试矩阵至少覆盖：无 ACL、无关目录 ACL、真实目录局部缩/扩权、无库共享根、上传分块绑定路径、公共链接、C/Go 最终写入、SQL/Hub 故障、原生整库同步限制、C Go 选择。构建、CI、DEV/PROD 状态分别报告，不能混称完成。
