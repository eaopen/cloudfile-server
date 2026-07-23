-- CloudFile schema additions to seafile-db (SQLite variant).
-- See ../mysql/cloudfile.sql for why these tables live here.

CREATE TABLE IF NOT EXISTS cf_dir_acl (id INTEGER PRIMARY KEY AUTOINCREMENT, repo_id CHAR(36) NOT NULL, path TEXT NOT NULL, path_hash CHAR(40) NOT NULL, subject_type VARCHAR(16) NOT NULL, subject VARCHAR(255) NOT NULL, permission VARCHAR(16) NOT NULL, inherit INTEGER NOT NULL DEFAULT 1, ctime BIGINT, mtime BIGINT);
CREATE UNIQUE INDEX IF NOT EXISTS cf_dir_acl_unique ON cf_dir_acl (repo_id, path_hash, subject_type, subject);
CREATE INDEX IF NOT EXISTS cf_dir_acl_repo ON cf_dir_acl (repo_id);
