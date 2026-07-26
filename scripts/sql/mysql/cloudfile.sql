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
