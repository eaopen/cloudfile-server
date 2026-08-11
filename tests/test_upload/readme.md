<!-- generated-by: gsd-doc-writer -->
# test_upload 手工并发上传工具

> 用途：说明 `test_upload.go` 的遗留手工运行方式和风险边界。
> 适用版本：CloudFile `dev` / Seafile CE 14 测试环境。
> 状态：暂停；未纳入 CI，不作为功能或性能验收证据。

工具通过 seaf-server RPC 获取上传 token，再并发向 fileserver 的 8082 端口上传固定
内容和文件名 `111.md`。它只打印 HTTP 状态和响应，不做断言、不清理远端文件，也
没有吞吐或延迟统计。

## 配置

复制 `account.conf` 到仓库外的临时位置，填写隔离测试环境：

```ini
[account]
server = http://127.0.0.1
username = test@example.invalid
password = replace-me
repoid = 00000000-0000-0000-0000-000000000000
thread_num = 10
```

不要把真实账号、密码或生产库 ID 提交到仓库。`-p` 参数接收 RPC socket 所在目录，
程序会在其下寻找 `seafile.sock`；目标 seaf-server 与 fileserver 必须已经运行。

## 手工运行

在 `tests/test_upload/` 中执行：

```bash
go run . -c /path/to/account.conf -p /path/to/runtime
```

先用低并发和专用测试库验证，运行后手工检查并删除 `111.md`。原说明中的
`accont.conf` 拼写错误已保存在
[doc/history/test-upload-readme.2022.md](../../doc/history/test-upload-readme.2022.md)。

恢复自动化前至少需要补：退出码断言、唯一文件名、清理逻辑、超时、结果统计和
受控测试环境配置。
