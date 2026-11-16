-- CloudFile V2 fresh schema. Native CE tables remain the authoritative facts.
-- This file is independent of cloudfile.sql and the legacy migration chain.

-- Store only explicitly enabled repository capabilities. Removing a row
-- disables that capability without changing storage routing or the other flag.
CREATE TABLE cf_repo_feature (
    repo_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    feature VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    PRIMARY KEY (repo_id, feature),
    CONSTRAINT repo_feature_name CHECK (feature IN ('checkout', 'protection'))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE cf_sso_group_map (
    provider VARCHAR(32) COLLATE utf8mb4_bin NOT NULL,
    subject_type VARCHAR(4) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    namespace VARCHAR(128) COLLATE utf8mb4_bin NOT NULL,
    external_id VARCHAR(225) COLLATE utf8mb4_bin NOT NULL,
    group_id BIGINT NOT NULL,
    PRIMARY KEY (provider,subject_type,namespace,external_id),
    UNIQUE KEY sso_native_group (group_id),
    CONSTRAINT sso_group_type CHECK (subject_type IN ('org','role')),
    CONSTRAINT sso_group_id CHECK (group_id > 0)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

CREATE TABLE cf_dir_acl (
    repo_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    path_hash BINARY(32) NOT NULL,
    path VARCHAR(4096) COLLATE utf8mb4_bin NOT NULL,
    subject_type VARCHAR(4) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    provider VARCHAR(32) COLLATE utf8mb4_bin NOT NULL,
    namespace VARCHAR(128) COLLATE utf8mb4_bin NOT NULL,
    external_id VARCHAR(225) COLLATE utf8mb4_bin NOT NULL,
    permission VARCHAR(9) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    inherit TINYINT NOT NULL,
    share_mode TINYINT NOT NULL,
    revision CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    PRIMARY KEY (repo_id,path_hash,subject_type,provider,namespace,external_id),
    KEY acl_shared_subject (provider,subject_type,namespace,external_id,share_mode,repo_id),
    CONSTRAINT acl_subject_type CHECK (subject_type IN ('user','org','role')),
    CONSTRAINT acl_permission CHECK (permission IN ('r','rw','invisible')),
    CONSTRAINT acl_inherit CHECK (inherit IN (0,1)),
    CONSTRAINT acl_share CHECK (share_mode IN (0,1) AND
        (share_mode=0 OR (permission IN ('r','rw') AND path <> '/')))
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
