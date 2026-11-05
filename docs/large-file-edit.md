# 大文件本地编辑：阶段一

读取、全量保存和状态恢复统一支持至 1 GiB（含边界）。超过上限返回
HTTP 413 / FILE_TOO_LARGE，并包含 size、max_size；文件对象读取失败仍为 503。
读取响应增加 X-CF-Max-File-Size，便于消费者识别实际服务端限制。

必须配套 Agent 的大文件修改；旧 Agent 仍会拒绝超过 128 MiB 的文件。
本阶段保留服务端完整临时文件校验，客户端采用文件流和有界快照缓冲；
不代表 CDC 已启用。单次大文件会占用服务端和客户端临时磁盘，需部署验收。

定向验证：TestCFEdit*（包含旧边界、新上限及超限错误），go vet ./...。
尚未执行真实 CAD / Windows / 慢速网络 / 生产部署验收。
下一阶段在当前 V2 授权及最终 CAS/ACL/Guard 事务内增加 CDC，不恢复旧扩展表。

## 阶段二：V2 CDC 协议

原始 read 改为先检查块尺寸，再按原生顺序流式输出，取消服务端整文件临时落盘；响应开始后的错误中断 HTTP，Agent 必须完成长度核验才接受工作副本。

新增入口均由现有 `/cf-edit/v1/*` scoped JWT、当前路径权限和只读模式限制保护：

| 入口 | 请求/结果 |
| --- | --- |
| GET capabilities | cdc_v1=true、max_file_size=1 GiB、max_block_size=10 MiB、max_blocks=256 |
| POST cdc-plan | 有序 manifest，返回 GC 代际及 missing 索引；不探测未经证明的任意哈希 |
| POST cdc-block | 二进制块及 X-CF-Base-ID / X-CF-GC-ID / X-CF-Block-ID，校验长度/SHA-1，返回 scoped HMAC receipt |
| POST cdc-publish | manifest + 原 X-CF-Action/Guard 头；校验块证明、尺寸、SHA-1、整文件 SHA-256，再进入共用提交事务 |

manifest 为 `{base,size,sha256,gc,blocks:[{id,size,receipt}]}`，最大 128 KiB，最多 256 块；顺序及重复块保留。原生 CDC 参数为 6/8/10 MiB，兼容 Seafile V1 FS/Block 格式。现有 Web 上传可能为固定分块，首次 CDC 保存不承诺高复用率。

块复用范围是当前目标文件基线，或调用者已经上传的块。回执以现有服务密钥、独立域标签签名，绑定主体/UID/资料库/路径/模式/基线/GC/块ID/大小；回执不是独立访问凭据。每次请求仍验证票据及权限，不增加数据库表。

最终发布使用原 ACL proof、Branch CAS、Guard 及审计事务；全量上传在原生分块前捕获 GCID；全量与 CDC 均在 Branch 前强制锁定原生 GCID 并比较代际，变化即拒绝。未发布块交由既有原生 GC 管理。全量路径保留供小文件及旧服务协商回退。

验证：Go CF/Cf 定向回归、go vet、真实原生 Rabin 向量探针（`bash tests/cf-edit-cdc/run.sh`）、原生 NewSeafile ID 对照、清单边界、回执篡改/跨主体/跨路径/GC 失效、禁止任意哈希存在性探测、HTTP 路由未授权拒绝、流式读取截断/溢出/存储故障。

限制：尚未做真实 SQL/对象存储/GC 并发与 CAD 跨仓联验，代码合并不代表部署验收通过。Agent 的协议桩不能替代这些验收。必须先部署兼容 Server，再发布 Agent；本任务不修改运行环境、代理配置或生产部署。fileserver.go 已在 Docker 上游改动清单中，无新增上游文件类别。

## 审查修复

共享提交函数的源码契约同时验证全量与 CDC 接线，保留 ACL 预检在事务前、最终 ACL 在事务内、单次 Hub 写裁决三项不变量。GC 代际参数改为必需值，禁止通过 nil 绕过；读取代际失败返回 GC_UNAVAILABLE，代际变化返回 GC_CHANGED。

此次使用启用中的 quick.yml 与本地定向 Go/static/native-vector 检查；未启用手动禁用的工作流。真实数据库/GC 并发、CAD、权限撤销、磁盘不足及 Windows 使用验收仍属于部署前待办。
