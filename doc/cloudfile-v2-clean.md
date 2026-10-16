# CloudFile V2 clean native Server base

This branch starts exactly from upstream haiwen/seafile-server `b6179eb3d73b04c67a00412ebaf964153c4ad2c0` (2026-10-15). No legacy CloudFile C/Go code or schema migration is copied.

The first reviewed additive asset is `scripts/sql/mysql/cloudfile-v2.sql`, containing only the confirmed three extension tables: `cf_library_storage`, `cf_dir_acl`, and `cf_sso_group_map`. Native `RepoStorageId`, Repo/FS/Commit/Block, permissions and users remain CE facts. The SQL **does not yet enable V2 ACL in Server/Fileserver**; that requires a separately reviewed small native integration for read/write/final-commit checks. Never expose Etech scoped access before those checks are demonstrated.

Source of approved SQL: archived V2 M1 snapshot. No existing deployment, data, or v0.9 branch is modified. The matching Docker `dev2-clean` source assembly must pin the exact upstream SHA and this branch's overlay.

## Automatic Local Library Storage

This optional native integration uses only `cf_library_storage` and CE's
`RepoStorageId`. It does not load the historical multi-storage, S3, task, or
extension-registration framework. Key validation follows the previously tested
`auto-local:` convention; routing and lifecycle code are independent V2 code.

With no setting, native filesystem paths remain in use and these code paths do
not query the extension schema. To allocate new libraries under an existing,
deployment-owned absolute directory, configure the same file in Server,
Fileserver and GC:

```ini
[cloudfile_v2]
auto_local_root=/srv/cloudfile-libraries
```

The directory must already exist. The root and key boundary cannot be symlinks;
its ancestors and contents must be writable only by trusted deployment tooling
and the service account. Each new library reserves its UUID as a never-reused
key, transactionally with `RepoStorageId.storage_id=auto-local:<key>`, before
writing its first commit. Objects use
`<root>/<key>/storage/{commits,fs,blocks}/<repo_id>/...`. C and Go use the same
committed binding. Block staging stays on the destination filesystem; C copies
between different filesystems publish complete staged objects.

Existing libraries with no binding retain their native paths; no library is
adopted or moved. Missing tables, invalid mappings, unsupported storage IDs,
missing mounts and symlink keys reject I/O rather than fall back. This minimal
implementation does not cache routes, and does not establish large-library
performance. Configure storage consistently before creating libraries; removing
this setting does not migrate already-routed data back into the native tree.

Soft deletion and trash restoration preserve the route. Permanent deletion
marks the key `retired`, retaining both key and native route so cold GC can
remove objects from the correct location. Failed creation may leave a reserved
binding or directory for inspection; it must not be automatically reused or
deleted. Existing data, shares and repo IDs are not rewritten.

`go test -race ./objstore` covers native/routed byte separation and route
failures. The Docker V2 runtime suite additionally exercises mixed libraries,
cross-language bytes, mapping failure, trash restore and cold GC. Their success
must be recorded for the exact source pins. These storage checks do not prove
directory ACL, shared-root access, browser OIDC, or Etech MVP acceptance.
