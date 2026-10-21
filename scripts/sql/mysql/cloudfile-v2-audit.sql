-- Current CloudFile V2-only supplementary operation audit.
-- Native Seafile Activity is authoritative for file and directory commits.
-- No outbox, migration ledger, server-side editing sessions or audit workers.
CREATE TABLE cf_audit_event (
    id BIGINT NOT NULL AUTO_INCREMENT,
    repo_id VARCHAR(36) COLLATE utf8mb4_bin NOT NULL,
    object_type VARCHAR(32) COLLATE utf8mb4_bin NOT NULL,
    object_id VARCHAR(64) COLLATE utf8mb4_bin NOT NULL,
    operation VARCHAR(32) COLLATE utf8mb4_bin NOT NULL,
    operator VARCHAR(255) COLLATE utf8mb4_bin NOT NULL,
    source VARCHAR(16) COLLATE utf8mb4_bin NOT NULL,
    `before` TEXT NULL,
    `after` TEXT NULL,
    source_path VARCHAR(1000) COLLATE utf8mb4_bin NULL,
    target_path VARCHAR(1000) COLLATE utf8mb4_bin NULL,
    path_hash BINARY(32) NULL,
    commit_id CHAR(40) CHARACTER SET ascii COLLATE ascii_bin NULL,
    result VARCHAR(16) COLLATE utf8mb4_bin NOT NULL,
    failure_reason TEXT NULL,
    occurred_at DATETIME(6) NOT NULL,
    PRIMARY KEY (id),
    KEY cf_audit_repo_time (repo_id, occurred_at, id),
    KEY cf_audit_path_time (repo_id, path_hash, occurred_at, id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
