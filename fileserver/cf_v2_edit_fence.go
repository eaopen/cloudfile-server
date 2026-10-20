package main

// CF-EDIT V2 safety fence at the *final* native Branch transaction.
// This denies unscoped native writes to checked-out paths. It does not grant
// controlled publication; that requires the reviewed Guard+Intent native seam.
// This file intentionally does not duplicate CF-ACL or Checkout authorization.
import (
	"context"
	"database/sql"
	"errors"
	"strings"

	"github.com/haiwen/seafile-server/fileserver/commitmgr"
	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/option"
)

var errCFEditGuard = errors.New("CF-EDIT: native write touches a checked-out file or edit state is unavailable")

func cfV2EditTouchesPath(entry *diff.DiffEntry, guardPath string) bool {
	for _, value := range []string{entry.Name, entry.NewName} {
		if value == "" {
			continue
		}
		changed := "/" + strings.TrimPrefix(value, "/")
		if changed == guardPath || strings.HasPrefix(guardPath, changed+"/") {
			// Covers file edits and directory moves/deletes over checked-out files.
			return true
		}
	}
	return false
}

// Caller must already hold Branch(master) FOR UPDATE in this same transaction.
// SQL errors and native diff failures deny, never silently fall back to CE.
func cfV2EditCheckUnscopedBranch(ctx context.Context, tx *sql.Tx, repoID, oldCommitID, newCommitID string) error {
	// Ordinary upload must not depend on Guard schema or SQL when Checkout
	// is disabled. Enabled-but-unavailable is an error, never a bypass.
	if !option.CloudFileCheckoutEnabled {
		return nil
	}
	if !option.V2GuardAvailable {
		return errCFEditGuard
	}
	query, err := cfEditGuardQuery(ctx, tx)
	if err != nil {
		return errCFEditGuard
	}
	rows, err := tx.QueryContext(ctx, query, repoID)
	if err != nil {
		return errCFEditGuard
	}
	var paths []string
	for rows.Next() {
		var path string
		if err := rows.Scan(&path); err != nil {
			rows.Close()
			return errCFEditGuard
		}
		if path == "" || !strings.HasPrefix(path, "/") {
			rows.Close()
			return errCFEditGuard
		}
		if len(paths) >= 8192 {
			rows.Close()
			return errCFEditGuard // bounded scope; large cases require a targeted policy
		}
		paths = append(paths, path)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return errCFEditGuard
	}
	if len(paths) == 0 || oldCommitID == newCommitID {
		return nil
	}
	oldCommit, err := commitmgr.Load(repoID, oldCommitID)
	if err != nil {
		return errCFEditGuard
	}
	newCommit, err := commitmgr.Load(repoID, newCommitID)
	if err != nil {
		return errCFEditGuard
	}
	var changes []*diff.DiffEntry
	if err := diff.DiffCommits(oldCommit, newCommit, &changes, false); err != nil {
		return errCFEditGuard
	}
	for _, change := range changes {
		for _, protected := range paths {
			if cfV2EditTouchesPath(change, protected) {
				return errCFEditGuard
			}
		}
	}
	return nil
}

// The deployed Checkout store and the optional path-keyed Rust store share a
// table name but not a schema. Select by actual columns before issuing SQL;
// never turn a failed query into permission to skip an installed Guard.
func cfEditGuardQuery(ctx context.Context, tx *sql.Tx) (string, error) {
	rows, err := tx.QueryContext(ctx, "SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_guard'")
	if err != nil {
		return "", errCFEditGuard
	}
	columns := make(map[string]bool)
	for rows.Next() {
		var column string
		if rows.Scan(&column) != nil || len(columns) >= 64 {
			rows.Close()
			return "", errCFEditGuard
		}
		columns[column] = true
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return "", errCFEditGuard
	}
	has := func(names ...string) bool {
		for _, name := range names {
			if !columns[name] {
				return false
			}
		}
		return true
	}
	// Require the complete reviewed layout, matching Hub qualification. A
	// partial or mixed table must not be mistaken for a valid Guard store.
	pathStore := len(columns) == 15 && has("repo_id", "path_hash", "path", "guard_id", "generation", "owner_uid", "owner_native_user", "terminal_type", "terminal_name", "checkout_ip", "proof_digest", "base_file_id", "lease_expires_at", "created_at", "updated_at")
	resourceStore := len(columns) == 16 && has("resource_uid", "repo_id", "lifecycle_ref", "guard_id", "generation", "mode", "owner", "owner_native_user", "source", "holder", "credential_epoch", "proof_digest", "base_file_id", "pending_intent", "lease_until", "hard_expire_at")
	if pathStore && !resourceStore {
		return "SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) WHERE repo_id=? AND guard_id IS NOT NULL AND lease_expires_at>UTC_TIMESTAMP(6) FOR UPDATE", nil
	}
	if resourceStore && !pathStore {
		// Match the native Checkout barrier: every non-null Guard remains a
		// fence until its lifecycle is reconciled, including expired leases.
		// A missing, stale or foreign resource yields NULL and is refused by
		// the path scanner; an inner join would silently lose that fence.
		return "SELECT CASE WHEN r.repo_id=g.repo_id AND r.lifecycle_ref=g.lifecycle_ref AND r.kind='file' AND r.state='active' THEN r.path ELSE NULL END FROM cf_edit_guard g LEFT JOIN cf_resource r ON r.uid=g.resource_uid WHERE g.repo_id=? AND g.guard_id IS NOT NULL FOR UPDATE", nil
	}
	return "", errCFEditGuard
}
