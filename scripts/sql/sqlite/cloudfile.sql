-- CloudFile schema additions to seafile-db (SQLite variant).
-- See ../mysql/cloudfile.sql for why these tables live here.

CREATE TABLE IF NOT EXISTS cf_dir_acl (id INTEGER PRIMARY KEY AUTOINCREMENT, repo_id CHAR(36) NOT NULL, path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, subject_type VARCHAR(16) NOT NULL, subject VARCHAR(255) NOT NULL, permission VARCHAR(16) NOT NULL, inherit INTEGER NOT NULL DEFAULT 1, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_dir_acl_unique ON cf_dir_acl (repo_id, path_hash, subject_type, subject);
CREATE INDEX IF NOT EXISTS cf_dir_acl_repo ON cf_dir_acl (repo_id);

CREATE TABLE IF NOT EXISTS cf_dir_admin (id INTEGER PRIMARY KEY AUTOINCREMENT, repo_id CHAR(36) NOT NULL, path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, subject_type VARCHAR(16) NOT NULL, subject VARCHAR(255) NOT NULL, inherit INTEGER NOT NULL DEFAULT 1, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_dir_admin_unique ON cf_dir_admin (repo_id, path_hash, subject_type, subject);
CREATE INDEX IF NOT EXISTS cf_dir_admin_repo ON cf_dir_admin (repo_id);

CREATE TABLE IF NOT EXISTS cf_sso_group_map (id INTEGER PRIMARY KEY AUTOINCREMENT, provider VARCHAR(32) NOT NULL, external_id VARCHAR(255) NOT NULL, group_id INTEGER NOT NULL, name VARCHAR(255) NOT NULL, subject_type VARCHAR(16), parent_external_id VARCHAR(255), ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_group_map_unique ON cf_sso_group_map (provider, external_id);
CREATE UNIQUE INDEX IF NOT EXISTS cf_sso_group_map_group ON cf_sso_group_map (group_id);

CREATE TABLE IF NOT EXISTS cf_managed_library_share (id INTEGER PRIMARY KEY AUTOINCREMENT, provider VARCHAR(32) NOT NULL, repo_id CHAR(36) NOT NULL, external_group_id VARCHAR(128) NOT NULL, seafile_group_id INTEGER NOT NULL, permission VARCHAR(8) NOT NULL, state VARCHAR(16) NOT NULL, last_error VARCHAR(1000), ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_managed_library_share_unique ON cf_managed_library_share (provider, repo_id, external_group_id);

CREATE TABLE IF NOT EXISTS cf_library_share_revision (provider VARCHAR(32) NOT NULL, repo_id CHAR(36) NOT NULL, policy_revision BIGINT NOT NULL, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_library_share_revision_unique ON cf_library_share_revision (provider, repo_id);

(id INTEGER PRIMARY KEY AUTOINCREMENT, name VARCHAR(64) NOT NULL, last_run BIGINT, status VARCHAR(16) NOT NULL, detail TEXT);
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

CREATE TABLE IF NOT EXISTS cf_edit_session (session_id CHAR(36) NOT NULL PRIMARY KEY, ticket_digest CHAR(64) NOT NULL UNIQUE, ticket_expire_at BIGINT NOT NULL, mode VARCHAR(32) NOT NULL, username VARCHAR(255) NOT NULL, repo_id CHAR(36) NOT NULL, normalized_path TEXT NOT NULL, base_file_id CHAR(40), generation CHAR(36), state VARCHAR(16) NOT NULL, claimed_at BIGINT, closed_at BIGINT, created_at BIGINT NOT NULL, updated_at BIGINT NOT NULL);
CREATE INDEX IF NOT EXISTS cf_edit_session_expiry ON cf_edit_session (state, ticket_expire_at);
CREATE TABLE IF NOT EXISTS cf_fileop_task (id INTEGER PRIMARY KEY AUTOINCREMENT, task_id CHAR(36) NOT NULL, idempotency_key CHAR(64) NOT NULL, username VARCHAR(255) NOT NULL, operation VARCHAR(16) NOT NULL, status VARCHAR(16) NOT NULL, detail TEXT, ctime BIGINT NOT NULL, mtime BIGINT NOT NULL);
CREATE UNIQUE INDEX IF NOT EXISTS cf_fileop_task_idem ON cf_fileop_task (username, idempotency_key);
CREATE INDEX IF NOT EXISTS cf_fileop_task_id ON cf_fileop_task (task_id);

-- Retain stable keys after deletion: cold GC needs the physical directory,
-- and reusing a deleted key must never attach a new library to old objects.
CREATE TABLE IF NOT EXISTS cf_library_storage_key (
    repo_key VARCHAR(80) NOT NULL PRIMARY KEY,
    repo_id CHAR(36) NOT NULL UNIQUE
);
