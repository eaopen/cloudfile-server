[CloudFile product home / 产品总入口](https://github.com/eaopen/cloudfile) · [English](https://github.com/eaopen/cloudfile/blob/main/README.md) · [简体中文](https://github.com/eaopen/cloudfile/blob/main/README.zh-CN.md)

# CloudFile Server

CloudFile's file core, based on Seafile Community Edition. It handles file data, storage and permission enforcement. For installation and product guidance, start at the [CloudFile product home](https://github.com/eaopen/cloudfile).

CloudFile 基于 Seafile Community Edition 的文件核心，负责文件数据、存储与权限执行。安装和产品使用请从 [CloudFile 产品入口](https://github.com/eaopen/cloudfile)开始。

The upstream AGPLv3 license and OpenSSL linking permission remain unchanged; the `python/` directory uses Apache 2.0. See [LICENSE.txt](LICENSE.txt). The upstream introduction follows.

保留上游 AGPLv3 和 OpenSSL 链接附加许可，`python/` 目录采用 Apache 2.0，见 [LICENSE.txt](LICENSE.txt)。以下保留上游项目介绍。

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
