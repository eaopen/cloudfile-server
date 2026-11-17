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
- Repo 删除走独立管理链路，不经文件 Branch。V1 为避免竞态，**启用了 protection 的资料库一律禁止整库删除**；需要退库时，先解除该库全部定版、确认 `cf_file_protection` 已无该库残留行，再通过管理接口关闭该库的 protection（残留未清时接口返回 409），之后才能删除。
- 每次 Branch 发布都会读取 `cf_repo_feature`（旧版"未选定库跳过 SQL"的优化已随策略入库取消）；该表或保护表读取失败必须拒绝写入，不得当作"未启用"处理。
- SQL 按 path_hash 精确检查，并按路径的安全前缀检查受影响后代。最长 8192 Diff 项，超限拒绝，不能无限长时间持有 Branch 锁。

## 模型与部署

六张 V2 扩展表中，`cf_file_protection` 承载定版记录：主键为 (repo_id,path_hash)，索引为 (repo_id,path 前缀)。
DDL 在 Server 仓 scripts/sql/mysql/cloudfile-v2-protection.sql；Hub Schema 验证需要使用同一份 DDL。

库级能力（哪些资料库启用 protection / checkout）存放在同库的 `cf_repo_feature`，主键 (repo_id,feature)，
由 C/Go/Hub 在提交事务内读取。**这是运行期唯一事实源。**

seafile.conf 的 `[cloudfile] protection_repo_ids` / `checkout_repo_ids` 已退化为一次性迁移输入：
进程仍会读取它们（C 侧首次判定库级策略时、Go 侧启动时），非空即打印
`legacy migration input and is IGNORED at runtime`，但不再据此判定任何写入。
**只改这两行配置不会改变保护范围，也不会报错。**

库级开关通过管理接口修改，无需重启：

    POST /api/v2.1/admin/cloudfile/repos/<uuid>/features/
    {"protection": true}  /  {"protection": false}
    {"checkout": true}    /  {"checkout": false}

要求平台管理员且具备资料库管理权限；接口先锁 Branch(master) 再写表，返回提交后的实际状态。
Checkout 与 protection 走同一张表、同一接口：`checkout_enabled` 也已退化为迁移提示，不再参与
判定，库级行写入即生效，不需要任何部署级开关前置。

上线顺序：停写 -> 运行 tools/migrate/repo-features.py（可先不带 --apply 检查差异）-> 部署匹配的
Server/Hub/Docker 组合 -> 启动 -> 验收。

**不得跳过迁移**：`cf_repo_feature` 缺失时每一次 Branch 发布都 fail-closed，表现为该实例上所有
资料库的写入失败（上传/删除/改名/移动），读取不受影响。先换二进制后迁移会造成全站写故障。

不得回滚到不包含保护屏障的 Native Server。确需回滚到"读配置"的旧版时，必须先把
`cf_repo_feature` 中 `feature='protection'` 的库回写进 `seafile.conf` 的 `protection_repo_ids`，
否则回滚后保护范围为空——数据库仍有定版记录，但没有任何进程再执行拦截。

数据库初始化不允许静默修改旧生产卷。

## API 合同

- GET /api/v2.1/cloudfile/protection/v1/status/?repo_id=...&path=/design/a.prt
- POST /api/v2.1/cloudfile/protection/v1/status/ : repo_id + paths (最多 200)
- POST /api/v2.1/cloudfile/protection/v1/protect/
- POST /api/v2.1/cloudfile/protection/v1/unprotect/

修改请求必须携带 repo_id、path、expected_file_id（小写40位原生文件 ID）。
Hub 通过权限校验、文件存在及版本 HEAD CAS，持有 native Branch(master) 锁，原子写入保护表和已有 cf_audit_event。
列表状态通过批量读取投影，不向原生 CE 标签写入 SYS_FILE_PROTECTED；普通标签复制不能传播不可变状态。

## 缺陷记录

### D1：关闭库级 protection 未校验残留定版记录（2026-11-16 已修复）

修复前，`POST /api/v2.1/admin/cloudfile/repos/<uuid>/features/` 收到 `{"protection": false}` 时
只删除 `cf_repo_feature` 中该库的 `protection` 行，**不检查 `cf_file_protection` 是否仍有该库的
记录**（Hub `cloudfile_ext/v2/http.py` 的 `RepoFeatures.post`）。

影响：

- 屏障立即消失：C/Go 判定 `protection=0`，不再做路径比对，已定版原件马上可被覆盖、删除、改名、
  移动，且不会再有任何拦截。
- 定版记录仍留在表里：状态查询继续返回 `protected=true`，界面继续显示"已定版"，与实际可写状态矛盾。
- 若关闭期间文件被改写，重新开启后屏障保护的是**被改写后的新内容**，不是当初定版的内容；
  原记录的 `file_id` 已过期。
- 重新开启后对该路径调用 `protect` 会持续返回 `PROTECTION_CONFLICT`（记录版本与当前 file_id 不一致），
  必须先 `unprotect` 清掉残留行再重新 `protect`。

触发条件：仅平台管理员显式关闭；单个文件的 protect/unprotect 不受影响。

修复：`RepoFeatures.post` 在删除能力行之前、于同一事务与同一把 Branch(master) 锁内查询
`cf_file_protection`，存在残留行即返回 409
`Protected files must be unprotected before disabling protection for this library.` 并保持原状。
校验位于所有写操作之前，因此同一请求里若还带 `checkout` 变更，不会出现"checkout 已落库、
protection 被拒"的部分应用。

运维顺序不变：关闭某库保护前仍须逐文件解除定版；接口只在还有残留行时拒绝，不会替人解除。
未升级到含此修复的 Hub 时，该缺陷仍存在，必须人工自检：

    SELECT 1 FROM cf_file_protection WHERE repo_id = '<uuid>' LIMIT 1;

## 合并前必须通过

1. Go/C/Hub 真正连同一 MariaDB 和匹配代码运行；并发上传/定版不得跨事务竞态绕过。
2. 覆盖、删除原件、父目录删除、移动/改名、回收站恢复、整库删除均被有效拦截。
3. 普通原生复制读源、向有写权限的新路径创建副本，不会自动继承保护。
4. 旧 Agent2 已签票据、Web、API、同步、WebDAV 路径不得绕过；各类写入返回明确错误。
5. 每次发布读取 `cf_repo_feature` 的固定开销已测量并在预算内（策略入库后不再有"未启用库零查询"，旧基线作废）；受保护文件数量增加时提交耗时基准合格，且"边提交边改库级开关"不出现锁等待超时或死锁。
6. 合并前明确 Docker 三仓 SHA 配对和数据库迁移/回滚预案。

本地 Go 定向测试仅覆盖 SQL 逻辑/错误类型，不能替代 C/Linux 完整构建及真实 MySQL 竞态检查。
