[English](README.md) | [简体中文](README.zh-CN.md)

# CloudFile Server

CloudFile's file core, based on Seafile Community Edition. It handles file data, storage and permission enforcement. For installation and product guidance, start at the [CloudFile product home](https://github.com/eaopen/cloudfile).

The upstream AGPLv3 license and OpenSSL linking permission remain unchanged; the `python/` directory uses Apache 2.0. See [LICENSE.txt](LICENSE.txt). The upstream introduction follows.

---

Seafile Server Core
============

Seafile is an open source cloud storage system with features on privacy protection and teamwork. Collections of files are called libraries, and each library can be synced separately. A library can also be encrypted with a user chosen password. Seafile also allows users to create groups and easily sharing files into groups.

This is the core component of Seafile server. It provides RPC to the web front-end (Seahub) to access files, and provides HTTP APIs to the desktop clients for syncing files.

Build and Run
=============

CloudFile build instructions: [Docker build guide](https://github.com/eaopen/cloudfile-docker/blob/dev/build/README.md).
Upstream reference: <https://manual.seafile.com/13.0/develop/server/> (for the documented upstream version).

Contributing
===========

For more informations read [Contribution](https://manual.seafile.com/latest/introduction/contribution/).

License
=======

The Seafile server core is published under AGPLv3. Other components of Seafile have different licenses. Please refer to the coresponding projects.

Contact
=======

Twitter: @seafile <https://twitter.com/seafile>

Forum: <https://forum.seafile.com>
