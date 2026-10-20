[CloudFile product home / 产品总入口](https://github.com/eaopen/cloudfile) · [English](https://github.com/eaopen/cloudfile/blob/main/README.md) · [简体中文](https://github.com/eaopen/cloudfile/blob/main/README.zh-CN.md)

# CloudFile Server

> **当前开发：默认分支 `dev` = CloudFile V2**（以 Seafile CE14 为上游基础）。不是历史 V0.x，也不是已归档的 `dev2` / `dev2-clean`。新功能用 `feat/<功能>`，修复用 `fix/<问题>`，文档用 `docs/<主题>`。不需要额外的版本前缀。

CloudFile's file core, based on Seafile Community Edition. It handles file data, storage and permission enforcement. For installation and product guidance, start at the [CloudFile product home](https://github.com/eaopen/cloudfile).

CloudFile 基于 Seafile Community Edition 的文件核心，负责文件数据、存储与权限执行。与 [CloudFile Hub](https://github.com/eaopen/cloudfile-hub) 配合，由 [CloudFile Docker](https://github.com/eaopen/cloudfile-docker) 负责源码组装、镜像与部署。

**跨仓构建需固定 Server、Hub、Docker 的 Commit SHA；`dev` 表示开发线，不表示已发布或已部署。** 日常文档更新不编译、不跑测试；代码只按影响范围做必要验证，完整构建在明确的集成/发布阶段进行。参见 [AGENTS.md](AGENTS.md)、[Docker 分支规则](https://github.com/eaopen/cloudfile-docker/blob/dev/BRANCHING.md)、[Server 测试指南](README.testing.md)。

The upstream AGPLv3 license and OpenSSL linking permission remain unchanged; the `python/` directory uses Apache 2.0. See [LICENSE.txt](LICENSE.txt).

下文是保留的 **Seafile 上游介绍**，并非 CloudFile V2 的分支或构建指令。

---

Seafile Server Core [![Build Status](https://secure.travis-ci.org/haiwen/seafile-server.svg?branch=master)](http://travis-ci.org/haiwen/seafile-server)
============

Seafile is an open source cloud storage system with features on privacy protection and teamwork. Collections of files are called libraries, and each library can be synced separately. A library can also be encrypted with a user chosen password. Seafile also allows users to create groups and easily sharing files into groups.

This is the core component of Seafile server. It provides RPC to the web front-end (Seahub) to access files, and provides HTTP APIs to the desktop clients for syncing files.

Build and Run
=============

See <https://manual.seafile.com/build_seafile/server/>

Contributing
===========

For more informations read [Contribution](https://manual.seafile.com/contribution/).

License
=======

The Seafile server core is published under AGPLv3. Other components of Seafile have different licenses. Please refer to the coresponding projects.

Contact
=======

Twitter: @seafile <https://twitter.com/seafile>

Forum: <https://forum.seafile.com>
