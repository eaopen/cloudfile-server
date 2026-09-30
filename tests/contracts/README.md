# 公共 ACL 验收向量

`acl.json` 由 EAP 规范导出，只包含虚构主体的 ACL 输入与预期结果，随 Server 提交固定版本。公共 CI 和完整构建测试无需检出私有产品文档库或配置跨仓凭据。

修改规范后，在同级 `eap-cloudfile` 执行 `python3 tools/export_test_contracts.py`，同时提交更新的公共向量；执行 `python3 tools/export_test_contracts.py --check` 校验规范与实现测试使用相同向量。测试不静默回退到其他文件或跳过缺失向量。
