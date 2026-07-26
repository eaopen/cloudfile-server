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
  ctime BIGINT,
  mtime BIGINT,
  UNIQUE INDEX cf_sso_group_map_unique (provider, external_id),
  UNIQUE INDEX cf_sso_group_map_group (group_id)
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
