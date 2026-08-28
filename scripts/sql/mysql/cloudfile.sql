-- CloudFile schema additions to seafile-db.
--
-- Kept in its own file rather than appended to seafile.sql so there is exactly
-- one definition of the cf_* tables, and so following upstream never conflicts
-- on this file. It is applied by the docker bootstrap on every start
-- (scripts_14.0/bootstrap.py, apply_cloudfile_schema), which covers fresh
-- installs, version upgrades and an existing CE deployment adopting CloudFile
-- alike -- every statement is IF NOT EXISTS.
--
-- These live in seafile-db, not seahub-db, because seaf-server and the Go
-- fileserver both have to read them to enforce ACL for WebDAV and the desktop
-- sync client, and neither of them connects to seahub-db. Seahub reaches them
-- through a second connection (cloudfile_ext.db_router) with managed=False
-- models, so Django migrations never own this schema.
--
-- Semantics: cloudfile-docker/docs/acl-semantics.md

CREATE TABLE IF NOT EXISTS cf_dir_acl (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  repo_id CHAR(36) NOT NULL,
  path VARCHAR(1000) NOT NULL,
  -- sha1(path), indexed instead of `path` because MySQL cannot index a
  -- 1000-character utf8mb4 column.
  path_hash CHAR(40) NOT NULL,
  subject_type VARCHAR(16) NOT NULL,
  subject VARCHAR(255) NOT NULL,
  permission VARCHAR(16) NOT NULL,
  inherit TINYINT NOT NULL DEFAULT 1,
  ctime BIGINT,
  mtime BIGINT,
  UNIQUE INDEX cf_dir_acl_unique (repo_id, path_hash, subject_type, subject),
  INDEX cf_dir_acl_repo (repo_id)
) ENGINE=INNODB;

-- Directory-level admin (delegated manage): the orthogonal dimension to
-- cf_dir_acl (acl-semantics.md section 7). A row grants the subject the right
-- to manage ACL rules -- and further admin grants -- on `path` and, with
-- inherit, everything below it. There is no permission column: the grant *is*
-- the admin role. Only the Hub reads this table -- management is a Hub-side
-- decision and seaf-server enforces content, not manage -- but it lives here
-- because this is the one schema mechanism for cf_* tables and it runs on
-- every start, covering fresh installs and existing deployments alike.
CREATE TABLE IF NOT EXISTS cf_dir_admin (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  repo_id CHAR(36) NOT NULL,
  path VARCHAR(1000) NOT NULL,
  path_hash CHAR(40) NOT NULL,
  subject_type VARCHAR(16) NOT NULL,
  subject VARCHAR(255) NOT NULL,
  inherit TINYINT NOT NULL DEFAULT 1,
  ctime BIGINT,
  mtime BIGINT,
  UNIQUE INDEX cf_dir_admin_unique (repo_id, path_hash, subject_type, subject),
  INDEX cf_dir_admin_repo (repo_id)
) ENGINE=INNODB;

-- SSO directory mapping: which Seafile groups CloudFile created, mirroring
-- which groups in the customer's directory.
--
-- Semantics: cloudfile-docker/docs/sso-mapping.md
--
-- Unlike cf_dir_acl, nothing below the Hub reads these -- group membership is
-- enforced through ccnet, which every layer already consults. They are here
-- because this is the one schema mechanism for cf_* tables, applied on every
-- start; a second home in seahub-db would mean a Django migration history to
-- carry across upstream merges in exchange for nothing.
--
-- external_id is a string, not a number: OIDC group claims and LDAP DNs both
-- are. That is also why upstream's external_department table cannot be reused
-- here -- its outer_id is a BIGINT.
CREATE TABLE IF NOT EXISTS cf_sso_group_map (
  id BIGINT NOT NULL PRIMARY KEY AUTO_INCREMENT,
  provider VARCHAR(32) NOT NULL,
  external_id VARCHAR(255) NOT NULL,
  -- Unique: a Seafile group is mirrored from at most one directory group.
  -- Two mappings onto one group would fight over its membership every tick.
  group_id INT NOT NULL,
  name VARCHAR(255) NOT NULL,
  -- Hierarchy contract (eap-cloudfile decision 2026-08-27 §3): 'dept' rows
  -- are created as Seafile departments (parent_group_id -1 or >0), 'group'
  -- rows stay flat. Nullable so rows written before the upgrade keep reading
  -- as plain groups; CREATE TABLE IF NOT EXISTS never alters an existing
  -- table, and NULL is the pre-upgrade value.
  subject_type VARCHAR(16) NULL,
  -- external_id of the parent dept, resolved to a Seafile group_id at apply
  -- time from rows the same sync writes. Never a numeric Seafile id: external
  -- ids survive a Seafile rebuild, numeric ids do not.
  parent_external_id VARCHAR(255) NULL,
  ctime BIGINT,
  mtime BIGINT,
  UNIQUE INDEX cf_sso_group_map_unique (provider, external_id),
  UNIQUE INDEX cf_sso_group_map_group (group_id)
) ENGINE=INNODB;

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
) ENGINE=INNODB;

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
) ENGINE=INNODB;

-- External resource sources: an SMB/NFS share the operator has already
-- mounted on the host and bind-mounted into the container, registered here so
-- it can be browsed from CloudFile.
--
-- Semantics: cloudfile-docker/docs/external-sources.md
--
-- Like cf_sso_group_map, nothing below the Hub reads these -- external sources
-- deliberately never enter the repo/commit/block model, so seaf-server and the
-- Go fileserver have nothing to enforce here. They live in seafile-db for the
-- same reason: this is the one schema mechanism for cf_* tables and it runs on
-- every start, whereas a second home in seahub-db would mean carrying a Django
-- migration history across upstream merges in exchange for nothing.
--
-- repo_id is a synthetic UUID that matches no real library. It exists so that
-- cf_dir_acl rules can be written against a source's subdirectories with no
-- new code, and so the shadow layer (docs/external-sources.md section six) has
-- an id to present. It is NOT a foreign key into Repo, and nothing may treat it
-- as one.
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
) ENGINE=INNODB;

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
) ENGINE=INNODB;

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
) ENGINE=INNODB;

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
) ENGINE=INNODB;

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
) ENGINE=INNODB;

-- File-lock truth for manual checkout, local editors and OnlyOffice.  CE's
-- FileLocks table has no manager or write-path enforcement, so it is never
-- written at runtime.  A lease is keyed by the normalized object path and a
-- fresh UUID generation is produced every time an expired/released row is
-- claimed; old sessions therefore cannot become valid again after a release.
CREATE TABLE IF NOT EXISTS cf_lock_lease (
  repo_id CHAR(36) NOT NULL,
  normalized_path VARCHAR(1000) NOT NULL,
  -- SHA1 is indexed instead of the full utf8mb4 path; all reads also compare
  -- normalized_path so a theoretical digest collision cannot alias a lock.
  path_hash CHAR(40) NOT NULL,
  lock_id CHAR(36) NOT NULL,
  generation CHAR(36) NOT NULL,
  owner VARCHAR(255) NOT NULL,
  kind VARCHAR(32) NOT NULL,
  session_id CHAR(36),
  device_id VARCHAR(255),
  source_file_id CHAR(40),
  source_commit_id CHAR(40),
  lease_until BIGINT NOT NULL,
  hard_expire_at BIGINT NOT NULL,
  last_heartbeat_at BIGINT,
  status VARCHAR(16) NOT NULL,
  forced_by VARCHAR(255),
  forced_reason TEXT,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  PRIMARY KEY (repo_id, path_hash),
  INDEX cf_lock_lease_live (repo_id, status, lease_until)
) ENGINE=INNODB;

-- A monotonic, opaque value for clients that poll the lock set.  Lease
-- refreshes do not update this value; acquire/release state transitions do.
CREATE TABLE IF NOT EXISTS cf_lock_repo_revision (
  repo_id CHAR(36) NOT NULL PRIMARY KEY,
  revision BIGINT NOT NULL,
  updated_at BIGINT NOT NULL
) ENGINE=INNODB;

-- Opaque, single-claim sessions for CloudFile Local. The descriptor downloaded
-- by a browser carries only the ticket; access and write-back capabilities are
-- minted after the native agent claims it and never persist in this table.
CREATE TABLE IF NOT EXISTS cf_edit_session (
  session_id CHAR(36) NOT NULL PRIMARY KEY,
  ticket_digest CHAR(64) NOT NULL,
  ticket_expire_at BIGINT NOT NULL,
  mode VARCHAR(32) NOT NULL,
  username VARCHAR(255) NOT NULL,
  repo_id CHAR(36) NOT NULL,
  normalized_path VARCHAR(1000) NOT NULL,
  base_file_id CHAR(40),
  generation CHAR(36),
  state VARCHAR(16) NOT NULL,
  claimed_at BIGINT,
  closed_at BIGINT,
  created_at BIGINT NOT NULL,
  updated_at BIGINT NOT NULL,
  UNIQUE INDEX cf_edit_session_ticket (ticket_digest),
  INDEX cf_edit_session_expiry (state, ticket_expire_at)
) ENGINE=INNODB;

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
) ENGINE=INNODB;
