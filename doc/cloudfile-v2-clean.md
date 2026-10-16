# CloudFile V2 clean native Server base

This branch starts exactly from upstream haiwen/seafile-server `b6179eb3d73b04c67a00412ebaf964153c4ad2c0` (2026-10-15). No legacy CloudFile C/Go code or schema migration is copied.

The first reviewed additive asset is `scripts/sql/mysql/cloudfile-v2.sql`, containing only the confirmed three extension tables: `cf_library_storage`, `cf_dir_acl`, and `cf_sso_group_map`. Native `RepoStorageId`, Repo/FS/Commit/Block, permissions and users remain CE facts. The SQL **does not yet enable V2 ACL in Server/Fileserver**; that requires a separately reviewed small native integration for read/write/final-commit checks. Never expose Etech scoped access before those checks are demonstrated.

Source of approved SQL: archived V2 M1 snapshot. No existing deployment, data, or v0.9 branch is modified. The matching Docker `dev2-clean` source assembly must pin the exact upstream SHA and this branch's overlay.
