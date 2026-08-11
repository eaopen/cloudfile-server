-- CloudFile schema additions to seafile-db (SQLite variant).
-- See ../mysql/cloudfile.sql for why these tables live here.

CREATE TABLE IF NOT EXISTS cf_dir_acl (id INTEGER PRIMARY KEY AUTOINCREMENT, repo_id CHAR(36) NOT NULL, path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, subject_type VARCHAR(16) NOT NULL, subject VARCHAR(255) NOT NULL, permission VARCHAR(16) NOT NULL, inherit INTEGER NOT NULL DEFAULT 1, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_dir_acl_unique ON cf_dir_acl (repo_id, path_hash, subject_type, subject);
CREATE INDEX IF NOT EXISTS cf_dir_acl_repo ON cf_dir_acl (repo_id);
CREATE TABLE IF NOT EXISTS cf_dir_acl_repo_revision (repo_id CHAR(36) NOT NULL PRIMARY KEY, revision BIGINT NOT NULL, updated_at BIGINT NOT NULL);

CREATE TABLE IF NOT EXISTS cf_sso_group_map (id INTEGER PRIMARY KEY AUTOINCREMENT, provider VARCHAR(32) NOT NULL, external_id VARCHAR(255) NOT NULL, group_id INTEGER NOT NULL, name VARCHAR(255) NOT NULL, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_group_map_unique ON cf_sso_group_map (provider, external_id);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_group_map_group ON cf_sso_group_map (group_id);

CREATE TABLE IF NOT EXISTS cf_sso_sync_state (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(64) NOT NULL, last_run BIGINT, status VARCHAR(16) NOT NULL, detail TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_sync_state_name ON cf_sso_sync_state (name);

CREATE TABLE IF NOT EXISTS cf_search_index_state (id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(64) NOT NULL, last_activity_id BIGINT NOT NULL DEFAULT 0, last_run BIGINT, status VARCHAR(16) NOT NULL, detail TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_search_index_state_name ON cf_search_index_state (name);

CREATE TABLE IF NOT EXISTS cf_external_source (id INTEGER PRIMARY KEY AUTOINCREMENT, repo_id CHAR(36) NOT NULL, name VARCHAR(255) NOT NULL, source_type VARCHAR(32) NOT NULL, root_path TEXT NOT NULL, enabled INTEGER NOT NULL DEFAULT 1, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_external_source_repo ON cf_external_source (repo_id);
CREATE UNIQUE INDEX IF NOT EXISTS cf_external_source_name ON cf_external_source (name);

CREATE TABLE IF NOT EXISTS cf_external_source_grant (id INTEGER PRIMARY KEY AUTOINCREMENT, source_id BIGINT NOT NULL, subject_type VARCHAR(16) NOT NULL, subject VARCHAR(255) NOT NULL, permission VARCHAR(16) NOT NULL, ctime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_external_source_grant_unique ON cf_external_source_grant (source_id, subject_type, subject);
CREATE INDEX IF NOT EXISTS cf_external_source_grant_source ON cf_external_source_grant (source_id);

CREATE TABLE IF NOT EXISTS cf_external_scan_state (id INTEGER PRIMARY KEY AUTOINCREMENT, source_id BIGINT NOT NULL, cursor_path TEXT, last_run BIGINT, status VARCHAR(16) NOT NULL, detail TEXT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_external_scan_state_source ON cf_external_scan_state (source_id);

CREATE TABLE IF NOT EXISTS cf_external_overlay (id INTEGER PRIMARY KEY AUTOINCREMENT, source_id BIGINT NOT NULL, path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, metadata TEXT, tags TEXT, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_external_overlay_unique ON cf_external_overlay (source_id, path_hash);
CREATE INDEX IF NOT EXISTS cf_external_overlay_source ON cf_external_overlay (source_id);

CREATE TABLE IF NOT EXISTS cf_lock_lease (repo_id CHAR(36) NOT NULL, normalized_path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, lock_id CHAR(36) NOT NULL, generation CHAR(36) NOT NULL, owner VARCHAR(255) NOT NULL, kind VARCHAR(32) NOT NULL, session_id CHAR(36), device_id VARCHAR(255), source_file_id CHAR(40), source_commit_id CHAR(40), lease_until BIGINT NOT NULL, hard_expire_at BIGINT NOT NULL, last_heartbeat_at BIGINT, status VARCHAR(16) NOT NULL, forced_by VARCHAR(255), forced_reason TEXT, created_at BIGINT NOT NULL, updated_at BIGINT NOT NULL, PRIMARY KEY (repo_id, path_hash));
CREATE INDEX IF NOT EXISTS cf_lock_lease_live ON cf_lock_lease (repo_id, status, lease_until);
CREATE TABLE IF NOT EXISTS cf_lock_repo_revision (repo_id CHAR(36) NOT NULL PRIMARY KEY, revision BIGINT NOT NULL, updated_at BIGINT NOT NULL);
CREATE TABLE IF NOT EXISTS cf_edit_session (session_id CHAR(36) NOT NULL PRIMARY KEY, ticket_digest CHAR(64) NOT NULL UNIQUE, ticket_expire_at BIGINT NOT NULL, mode VARCHAR(32) NOT NULL, username VARCHAR(255) NOT NULL, repo_id CHAR(36) NOT NULL, normalized_path TEXT NOT NULL, base_file_id CHAR(40), generation CHAR(36), state VARCHAR(16) NOT NULL, claimed_at BIGINT, closed_at BIGINT, created_at BIGINT NOT NULL, updated_at BIGINT NOT NULL);
CREATE INDEX IF NOT EXISTS cf_edit_session_expiry ON cf_edit_session (state, ticket_expire_at);
