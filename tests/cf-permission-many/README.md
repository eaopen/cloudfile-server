# Legacy Search Multi-Check 测试

本目录只验证 transport 合并；scalar、C ACL、Hub hooks 仍逐路径执行两轮。

- `bash run.sh`：glib/jansson 环境下编译真实 helper、RPC wrapper、scalar/provider/ACL 函数；fixture 只替换存储、subject 加载和时钟。6 个测试组含参数化子用例。
- `bash build-native.sh`：需要已配置的 Linux native 构建树、libsearpc 与 libevhtp。默认复用邻仓 Docker 构建依赖，只读挂载；在新容器内编译并加入**测试专用** stderr scalar/C evaluate 计数，不修改工作树。输出 binary 不能发布。
- `CF_MANY_NATIVE_BINARY=/绝对路径/seaf-server bash run-native.sh`：启动独立 MySQL、Redis、native 容器，运行真实 RPC/parity/20 项 mutation/SQL 计数；自动删除容器与网络，日志保留在输出目录。可设置 `CF_MANY_NATIVE_OUTPUT` 指定证据路径。
- 完整契约与限制见邻仓 `cloudfile-docker/docs/legacy-search-permission-many.md`。真实测试明确使用 legacy schema，auth/ORM 由隔离适配器连接；不代表新版 4A schema 或完整 HTTP 登录测试。

SQL 计数使用独立数据库的 general_log；C 计数仅出现在测试构建。没有向生产加入 tracing，也不把 transport 次数冒充权限判断次数。完整无依赖的 Hub regression 可运行 `python -m pytest cloudfile_ext/search/tests`；具体 Django/pytest 设置以工作区现有测试环境为准。
