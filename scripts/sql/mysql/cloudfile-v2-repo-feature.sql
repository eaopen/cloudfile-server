-- Store only explicitly enabled repository capabilities. Removing a row
-- disables that capability without changing storage routing or the other flag.
CREATE TABLE cf_repo_feature (
    repo_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    feature VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    PRIMARY KEY (repo_id, feature),
    CONSTRAINT repo_feature_name CHECK (feature IN ('checkout', 'protection'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
