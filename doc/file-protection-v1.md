# CloudFile 文件定版保护 V1：部署与验收合同

状态：开发分支；尚未在 C/Go/Hub 及真实 MariaDB 上通过端到端验收，禁止单仓独立合并或直接部署。
技术基线：Seafile CE14 + CloudFile V2，不依赖 Pro。

## 功能语义

- 对选定资料库中的单个技术文件设置长期不可变状态。所有普通用户（含库管理员）不能覆盖、删除、重命名、移动原件或通过历史版本恢复覆盖它。
- 影响定版文件的上级目录删除、改名、移动均拒绝。V1 暂不允许同库目录改名后自动迁移保护路径。
- 原件允许预览、下载、普通复制；副本不继承保护，允许继续编辑。
- 文件保护与目录 ACL、Checkout 不同；原生 File ID 是定版时的内容校验，不是跨路径唯一身份。
- 仅平台系统管理员且具有资料库管理权限才能 Protect/Unprotect；非管理员的 File Protection 管理 API 直接拒绝。
- 这不是防数据库超级管理员篡改的 WORM 或合规归档承诺。

## 代码复用与最终执行

- C/Go 最终 Branch(master) 提交事务内检查稀疏保护状态，并拒绝覆盖/删除、目录结构影响及提交失败后的回退绕过。
- FileOp PREPARE 是提前提示，不是并发最终授权。
- Agent2 已签出的编辑凭证同样需要通过原生 cfEditCommit 的最终状态检查。
- Repo 删除走独立管理链路，不经文件 Branch。V1 为避免竞态，**保护范围内资料库一律禁止整库删除**；解除所有保护后，可在受控维护中从配置移除该库并完整重启，再删除。
- 未选中的资料库跳过 Protection SQL；选中资料库的保护表 SQL 故障必须拒绝相关写入。
- SQL 按 path_hash 精确检查，并按路径的安全前缀检查受影响后代。最长 8192 Diff 项，超限拒绝，不能无限长时间持有 Branch 锁。

## 模型与部署

新增第六张 V2 扩展表 cf_file_protection：主键为 (repo_id,path_hash)，索引为 (repo_id,path 前缀)。
DDL 在 Server 仓 scripts/sql/mysql/cloudfile-v2-protection.sql；Hub Schema 验证需要使用同一份 DDL。

在 C/Go/Hub 共用的 seafile.conf 的 [cloudfile] 节中添加：

    protection_repo_ids = 11111111-1111-4111-8111-111111111111

缺省未启用。不需要另外设置 Checkout 开关。

上线顺序：审查 DB DDL 和受控 schema 安装 -> 部署匹配的 Server/Hub/Docker 源码组合 -> 配置完整一致的 repo allowlist -> 全部进程重启 -> 验收。
不得在已有保护行的库撤掉 allowlist 或回滚旧版 Native Server。必要时先受控解除保护。数据库初始化不允许静默修改旧生产卷。

## API 合同

- GET /api/v2.1/cloudfile/protection/v1/status/?repo_id=...&path=/design/a.prt
- POST /api/v2.1/cloudfile/protection/v1/status/ : repo_id + paths (最多 200)
- POST /api/v2.1/cloudfile/protection/v1/protect/
- POST /api/v2.1/cloudfile/protection/v1/unprotect/

修改请求必须携带 repo_id、path、expected_file_id（小写40位原生文件 ID）。
Hub 通过权限校验、文件存在及版本 HEAD CAS，持有 native Branch(master) 锁，原子写入保护表和已有 cf_audit_event。
列表状态通过批量读取投影，不向原生 CE 标签写入 SYS_FILE_PROTECTED；普通标签复制不能传播不可变状态。

## 合并前必须通过

1. Go/C/Hub 真正连同一 MariaDB 和匹配代码运行；并发上传/定版不得跨事务竞态绕过。
2. 覆盖、删除原件、父目录删除、移动/改名、回收站恢复、整库删除均被有效拦截。
3. 普通原生复制读源、向有写权限的新路径创建副本，不会自动继承保护。
4. 旧 Agent2 已签票据、Web、API、同步、WebDAV 路径不得绕过；各类写入返回明确错误。
5. 未启用库没有额外数据库查询，受保护文件数量增加时提交耗时基准合格。
6. 合并前明确 Docker 三仓 SHA 配对和数据库迁移/回滚预案。

本地 Go 定向测试仅覆盖 SQL 逻辑/错误类型，不能替代 C/Linux 完整构建及真实 MySQL 竞态检查。
