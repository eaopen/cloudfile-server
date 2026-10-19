# CloudFile V2 clean native Server base

This branch starts exactly from upstream haiwen/seafile-server `b6179eb3d73b04c67a00412ebaf964153c4ad2c0` (2026-10-15). No legacy CloudFile C/Go code or schema migration is copied.

The first reviewed additive asset is `scripts/sql/mysql/cloudfile-v2.sql`, containing only the confirmed three extension tables: `cf_library_storage`, `cf_dir_acl`, and `cf_sso_group_map`. Native `RepoStorageId`, Repo/FS/Commit/Block, permissions and users remain CE facts. The SQL **does not yet enable V2 ACL in Server/Fileserver**; that requires a separately reviewed small native integration for read/write/final-commit checks. Never expose Etech scoped access before those checks are demonstrated.

Source of approved SQL: archived V2 M1 snapshot. No existing deployment, data, or v0.9 branch is modified. The matching Docker `dev2-clean` source assembly must pin the exact upstream SHA and this branch's overlay.

## Automatic Local Library Storage

This built-in native integration uses only `cf_library_storage` and CE's
`RepoStorageId`. It does not load the historical multi-storage, S3, task, or
extension-registration framework. Key validation follows the previously tested
`auto-local:` convention; routing and lifecycle code are independent V2 code.

Fresh V2 Docker sets a default library root automatically. For manually
assembled native deployments, `auto_local_root` selects an existing,
deployment-owned storage root; it is a **storage location**, not a product
feature flag. When upgrading a CE14 deployment with no configured root, keep
its native layout unchanged (no automatic data movement). Server, Fileserver
and GC must agree on the selected storage location:

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

The native FSCK/export flow and its offline mode remain unchanged. Offline
export continues to scan native data directories without a SQL connection.
Discovery/export of automatic library roots is a follow-up, outside the current
MVP; it is not yet supported by the native offline exporter. No database
prerequisite is added to offline recovery.

`go test -race ./objstore` covers native/routed byte separation and route
failures. The Docker V2 runtime suite additionally exercises mixed libraries,
cross-language bytes, mapping failure, trash restore and cold GC. Their success
must be recorded for the exact source pins. These storage checks do not prove
directory ACL, shared-root access, browser OIDC, or Etech MVP acceptance.

## Default-on native features (2026-10-17)

Checkout is installed with fresh V2 using the separate `cf_edit_guard` schema.
Both native C and Go implementations inspect the installed schema instead of
honoring the retired `[cloudfile] v2_edit_enabled` key. The Hub checks the
same schema. A reused CE14 instance without Guard can still perform native
file operations and optimistic edits; Guard installation requires explicit
migration and restarting all three processes. Once Guard exists, native write
fences cannot be disabled by configuration. Errors querying Guard fail closed.

Directory ACL always applies where the V2 ACL table contains protected rules;
no `v2_acl_enabled` option exists. CE Tag remains native. Meilisearch is an
optional external Search backend selected by configured endpoint and credential,
not a second feature-enable toggle. Keep external connections, storage paths,
security-sensitive identity settings, and bounded performance tuning separate
from feature flags. These source changes do not migrate running production data.
