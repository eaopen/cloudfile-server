-- Sparse, permanent protection for a file at one canonical native repository path.
-- No user ACL, lease, inheritance, or automatic tag binding. Only protected
-- originals have rows. Copying a file never copies this record.
CREATE TABLE cf_file_protection (
    repo_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    path_hash BINARY(32) NOT NULL,
    path VARCHAR(4096) COLLATE utf8mb4_bin NOT NULL,
    file_id CHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    protected_by VARCHAR(255) COLLATE utf8mb4_bin NOT NULL,
    protected_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (repo_id, path_hash),
    KEY cf_file_protection_prefix (repo_id, path(191))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
