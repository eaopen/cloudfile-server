-- Legacy CloudFile integration tables in seafile-db.
--
-- The Docker bootstrap applies this file on every start. Current directory
-- ACL, group mapping, share revision, lock, and local edit tables are created
-- by the Hub's versioned schema runner; defining older shapes here first would
-- prevent its migrations from succeeding. An already existing legacy table
-- remains untouched and requires an explicit migration.
--
-- These tables declare utf8mb4 explicitly so an older database default cannot
-- silently change their character support. Hub accesses them through its
-- separate seafile-db connection.

-- The Hub schema runner owns current directory ACL and admin tables. Native
-- authorization checks their schema before use and fail closed until migrated.

-- Library shares this integration applied, on behalf of an external system
-- (eap-cloudfile decision 2026-08-27 §4.3). The boundary this table draws is
-- the whole point: Seafile shares carry no marker of who created them, so a
-- reconcile loop that cannot tell its own work from a person's will
-- eventually delete a person's access. Only rows recorded here may be
-- revoked; a share in Seafile without a ledger row was made by hand and is
-- not ours to take back. external_group_id is the external system's stable
-- id (never a Seafile numeric id, which does not survive a rebuild);
-- seafile_group_id is re-resolved from cf_sso_group_map on every reconcile
-- and is diagnostic only.
CREATE TABLE IF NOT EXISTS cf_managed_library_share (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  provider VARCHAR(32) NOT NULL,
  repo_id CHAR(36) NOT NULL,
  external_group_id VARCHAR(128) NOT NULL,
  seafile_group_id INT NOT NULL,
  permission VARCHAR(8) NOT NULL,
  state VARCHAR(16) NOT NULL,
  last_error VARCHAR(1000) NULL,
  ctime BIGINT,
  mtime BIGINT,
  UNIQUE INDEX cf_managed_library_share_unique
    (provider, repo_id, external_group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- The Hub schema runner owns the current library-share revision ledger.

-- When the last sync ran and how it went. Directory mapping is eventually
-- consistent by design, and that trade is only defensible while "how stale is
-- this?" has an answer somebody can read.
CREATE TABLE IF NOT EXISTS cf_sso_sync_state (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(64) NOT NULL,
  last_run BIGINT,
  status VARCHAR(16) NOT NULL,
  detail TEXT,
  UNIQUE INDEX cf_sso_sync_state_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- External resource sources: an SMB/NFS share the operator has already
-- mounted on the host and bind-mounted into the container, registered here so
-- it can be browsed from CloudFile.
--
-- Semantics: cloudfile-docker/docs/external-sources.md
--
-- Nothing below the Hub reads these -- external sources deliberately never
-- enter the repo/commit/block model, so seaf-server and the Go fileserver
-- have nothing to enforce here.
--
-- repo_id is a synthetic UUID that matches no real library. It exists so the
-- shadow layer (docs/external-sources.md section six) has an id to present, and
-- so the directory-ACL *decision* path can be keyed by it. It is NOT a foreign
-- key into Repo, and nothing may treat it as one.
--
-- 修改逻辑/原因（2026-09-23 口径更正）：原文写 "cf_dir_acl rules can be written
-- against a source's subdirectories with no new code" —— **写路径不成立**。
-- AdminDirACLView（acl/admin_apis.py）与 DirACLView（acl/apis.py）在写入前都先
-- 校验 seafile_api.get_repo(repo_id)，合成 id 必然 404；可用的只有**判定**路径
-- （service.permission_for → 权限钩子，只查 cf_dir_acl，不调 seafile_api）。
-- 因此 v1 的外部源授权只到**源级 grant**；源内目录细化需先放行这两个写端点，
-- 改动清单见 cloudfile-docker/docs/features/external-sources.md 的「权限」一节。
--
-- root_path is a container path, and must resolve under
-- CF_EXTERNAL_SOURCES_ROOTS. That is enforced in the Hub on every access, not
-- just at registration time -- see external-sources.md section three for why
-- checking once is not enough.
CREATE TABLE IF NOT EXISTS cf_external_source (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  repo_id CHAR(36) NOT NULL,
  name VARCHAR(255) NOT NULL,
  source_type VARCHAR(32) NOT NULL,
  root_path VARCHAR(1000) NOT NULL,
  enabled TINYINT NOT NULL DEFAULT 1,
  ctime BIGINT,
  mtime BIGINT,
  UNIQUE INDEX cf_external_source_repo (repo_id),
  UNIQUE INDEX cf_external_source_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Who may read a source. External sources are not libraries: they have no
-- owner and are not shared through Seafile's own sharing, so authorisation is
-- its own table rather than a reuse of library-level shares.
--
-- permission has exactly one legal value ('r') in this release. The column
-- exists because read-only is a product decision rather than a property of the
-- data model, and adding a column later is more expensive than validating a
-- narrow domain now -- the validation lives in the API layer, not in a comment.
CREATE TABLE IF NOT EXISTS cf_external_source_grant (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  source_id BIGINT NOT NULL,
  subject_type VARCHAR(16) NOT NULL,
  subject VARCHAR(255) NOT NULL,
  permission VARCHAR(16) NOT NULL,
  ctime BIGINT,
  UNIQUE INDEX cf_external_source_grant_unique (source_id, subject_type, subject),
  INDEX cf_external_source_grant_source (source_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- How far cf-worker's incremental scan has walked each source (feature 51).
-- Created with the rest of the cluster's schema rather than when the scanner
-- lands, because the schema file is applied on every start and a table that
-- appears in a later release would otherwise only exist on deployments that
-- restarted after upgrading.
CREATE TABLE IF NOT EXISTS cf_external_scan_state (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  source_id BIGINT NOT NULL,
  -- Directory the last pass stopped at, so a large share is walked over
  -- several ticks instead of blocking the worker on one full traversal.
  cursor_path VARCHAR(1000),
  last_run BIGINT,
  status VARCHAR(16) NOT NULL,
  detail TEXT,
  UNIQUE INDEX cf_external_scan_state_source (source_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- User-facing metadata for an external path. This is deliberately a tiny
-- sidecar: metadata and tags can be edited without creating a Seafile commit
-- or copying a byte from the mounted share into the object store.
CREATE TABLE IF NOT EXISTS cf_external_overlay (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  source_id BIGINT NOT NULL,
  path VARCHAR(1000) NOT NULL,
  path_hash CHAR(40) NOT NULL,
  metadata TEXT,
  tags TEXT,
  ctime BIGINT,
  mtime BIGINT,
  UNIQUE INDEX cf_external_overlay_unique (source_id, path_hash),
  INDEX cf_external_overlay_source (source_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- How far cf-worker's search indexer has walked seafevents' Activity table.
-- One row per registered search provider that needs its own index built
-- (currently just 'meilisearch' -- SeaSearch is indexed by seafevents itself
-- and needs no row here). last_activity_id is Activity.id, which is a plain
-- monotonically increasing sequence in seahub-db, not one of the cf_* tables
-- -- safe to use as a resume cursor without owning that table.
--
-- Semantics: cloudfile-docker/docs/search.md
CREATE TABLE IF NOT EXISTS cf_search_index_state (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  name VARCHAR(64) NOT NULL,
  last_activity_id BIGINT NOT NULL DEFAULT 0,
  last_run BIGINT,
  status VARCHAR(16) NOT NULL,
  detail TEXT,
  UNIQUE INDEX cf_search_index_state_name (name)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- The Hub schema runner owns current file locks and local edit sessions.

-- Copy/move task idempotency and failure reporting (P2-06).  One row per
-- submitted copy/move intent; the idempotency_key is what turns a repeated
-- click into a no-op instead of a second copy.  Lives in seafile-db so the
-- Hub can deduplicate before ever calling seafile_api.copy_file / move_file,
-- which is the only layer that actually mutates the tree.  Nothing below the
-- Hub reads this: the write itself still goes through repo-op.c.
--
-- Semantics: cloudfile-docker/docs/features/fileops.md
--
-- idempotency_key is sha256(username|operation|src_repo|src_parent|src_names|
-- dst_repo|dst_parent), so two identical submissions map to the same task and
-- the second one is a no-op.
CREATE TABLE IF NOT EXISTS cf_fileop_task (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  task_id CHAR(36) NOT NULL,
  idempotency_key CHAR(64) NOT NULL,
  username VARCHAR(255) NOT NULL,
  operation VARCHAR(16) NOT NULL,
  status VARCHAR(16) NOT NULL,
  detail TEXT,
  ctime BIGINT NOT NULL,
  mtime BIGINT NOT NULL,
  UNIQUE INDEX cf_fileop_task_idem (username, idempotency_key),
  INDEX cf_fileop_task_id (task_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
