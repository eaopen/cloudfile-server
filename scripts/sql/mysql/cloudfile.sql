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
