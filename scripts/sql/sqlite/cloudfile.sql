-- CloudFile schema additions to seafile-db (SQLite variant).
-- See ../mysql/cloudfile.sql for why these tables live here.

CREATE TABLE IF NOT EXISTS cf_dir_acl (id INTEGER PRIMARY KEY AUTOINCREMENT, repo_id CHAR(36) NOT NULL, path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, subject_type VARCHAR(16) NOT NULL, subject VARCHAR(255) NOT NULL, permission VARCHAR(16) NOT NULL, inherit INTEGER NOT NULL DEFAULT 1, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_dir_acl_unique ON cf_dir_acl (repo_id, path_hash, subject_type, subject);
CREATE INDEX IF NOT EXISTS cf_dir_acl_repo ON cf_dir_acl (repo_id);

CREATE TABLE IF NOT EXISTS cf_sso_group_map (id INTEGER PRIMARY KEY AUTOINCREMENT, provider VARCHAR(32) NOT NULL, external_id VARCHAR(255) NOT NULL, group_id INTEGER NOT NULL, name VARCHAR(255) NOT NULL, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_group_map_unique ON cf_sso_group_map (provider, external_id);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_group_map_group ON cf_sso_group_map (group_id);

CREATE TABLE IF NOT EXISTS cf_sso_sync_state (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(64) NOT NULL, last_run BIGINT, status VARCHAR(16) NOT NULL, detail TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_sync_state_name ON cf_sso_sync_state (name);

CREATE TABLE IF NOT EXISTS cf_search_index_state (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(64) NOT NULL, last_activity_id BIGINT NOT NULL DEFAULT 0, last_run BIGINT, status VARCHAR(16) NOT NULL, detail TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_search_index_state_name ON cf_search_index_state (name);
