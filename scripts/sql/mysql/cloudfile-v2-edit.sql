-- CF-EDIT optional feature only. NOT part of the three-table V2 MVP schema.
-- Apply in seafile_db (InnoDB) on a fresh, isolated feature test instance.
-- Never import V0.9 migrations or write this to an existing PROD database.
--
-- One permanent row per physical file path serializes Lock and Checkout.
-- Branch -> Guard -> Intent is the single lock acquisition order.
-- A cleared guard_id keeps generation monotonic across re-checkouts.
CREATE TABLE cf_edit_guard (
    repo_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    path_hash BINARY(32) NOT NULL,
    path VARCHAR(4096) COLLATE utf8mb4_bin NOT NULL,
    guard_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL,
    generation BIGINT UNSIGNED NOT NULL DEFAULT 0,
    credential_epoch BIGINT UNSIGNED NOT NULL DEFAULT 0,
    mode VARCHAR(8) CHARACTER SET ascii COLLATE ascii_bin NULL,
    owner_uid VARCHAR(225) COLLATE utf8mb4_bin NULL,
    owner_native_user VARCHAR(255) COLLATE utf8mb4_bin NULL,
    client_type VARCHAR(16) CHARACTER SET ascii COLLATE ascii_bin NULL,
    client_id VARCHAR(128) COLLATE utf8mb4_bin NULL,
    client_ref VARCHAR(225) COLLATE utf8mb4_bin NULL,
    checkout_policy VARCHAR(12) CHARACTER SET ascii COLLATE ascii_bin NULL,
    proof_digest CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NULL,
    base_file_id CHAR(40) CHARACTER SET ascii COLLATE ascii_bin NULL,
    pending_intent CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL,
    lease_expires_at DATETIME(6) NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (repo_id, path_hash),
    UNIQUE KEY cf_edit_guard_id (guard_id),
    KEY cf_edit_guard_active (repo_id, guard_id),
    KEY cf_edit_guard_owner (owner_uid, guard_id),
    CONSTRAINT cf_edit_mode CHECK (mode IS NULL OR mode IN ('lock','checkout')),
    CONSTRAINT cf_edit_policy CHECK (checkout_policy IS NULL OR checkout_policy IN ('renewable','persistent')),
    CONSTRAINT cf_edit_active_shape CHECK (
        (guard_id IS NULL AND mode IS NULL AND owner_uid IS NULL AND
         proof_digest IS NULL AND pending_intent IS NULL)
        OR
        (guard_id IS NOT NULL AND mode IS NOT NULL AND owner_uid IS NOT NULL AND
         proof_digest IS NOT NULL AND checkout_policy IS NOT NULL AND
         (checkout_policy='persistent' OR lease_expires_at IS NOT NULL))
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;

-- One durable idempotency receipt for Agent optimistic edit and Checkout Commit/Checkin.
-- File/Block/Commit objects remain native CE; the intent never stores bytes.
CREATE TABLE cf_commit_intent (
    intent_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    repo_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    path_hash BINARY(32) NOT NULL,
    path VARCHAR(4096) COLLATE utf8mb4_bin NOT NULL,
    guard_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin NULL,
    generation BIGINT UNSIGNED NULL,
    credential_epoch BIGINT UNSIGNED NULL,
    actor_uid VARCHAR(225) COLLATE utf8mb4_bin NOT NULL,
    action VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    expected_file_id CHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    staged_file_id CHAR(40) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    content_digest CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    request_digest CHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    state VARCHAR(12) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    result_file_id CHAR(40) CHARACTER SET ascii COLLATE ascii_bin NULL,
    result_commit_id CHAR(40) CHARACTER SET ascii COLLATE ascii_bin NULL,
    created_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    updated_at DATETIME(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (intent_id),
    KEY cf_intent_file (repo_id, path_hash, created_at),
    KEY cf_intent_guard (guard_id, state),
    CONSTRAINT cf_intent_action CHECK (action IN ('optimistic','commit','checkin','checkin-unchanged')),
    CONSTRAINT cf_intent_state CHECK (state IN ('prepared','published','cancelled')),
    CONSTRAINT cf_intent_guard_shape CHECK (
        (action='optimistic' AND guard_id IS NULL AND generation IS NULL AND credential_epoch IS NULL)
        OR
        (action<>'optimistic' AND guard_id IS NOT NULL AND generation IS NOT NULL AND credential_epoch IS NOT NULL)
    )
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_bin;
