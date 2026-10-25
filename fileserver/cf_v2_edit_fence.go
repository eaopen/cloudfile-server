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

// Calculate the candidate change set before opening the final Branch
// transaction. OFF libraries do nothing. OPTIONAL repositories must be
// verifiable; a malformed candidate fails closed without acquiring a lock.
func cfV2EditPrepareUnscopedBranch(repoID, oldCommitID, newCommitID string) ([]*diff.DiffEntry, error) {
	if !option.CheckoutEnabledForRepo(repoID) {
		return nil, nil
	}
	if !option.V2GuardAvailable {
		return nil, errCFEditGuard
	}
	if oldCommitID == newCommitID {
		return []*diff.DiffEntry{}, nil
	}
	oldCommit, err := commitmgr.Load(repoID, oldCommitID)
	if err != nil {
		return nil, errCFEditGuard
	}
	newCommit, err := commitmgr.Load(repoID, newCommitID)
	if err != nil {
		return nil, errCFEditGuard
	}
	var changes []*diff.DiffEntry
	if err := diff.DiffCommits(oldCommit, newCommit, &changes, false); err != nil {
		return nil, errCFEditGuard
	}
	if changes == nil {
		return []*diff.DiffEntry{}, nil
	}
	return changes, nil
}

// The final native transaction only inspects the current Guard rows and
// precomputed candidate changes; it never loads commits or redoes the diff.
func cfV2EditCheckPreparedBranch(ctx context.Context, tx *sql.Tx, repoID string, changes []*diff.DiffEntry) error {
	if !option.CheckoutEnabledForRepo(repoID) {
		return nil
	}
	if !option.V2GuardAvailable || changes == nil {
		return errCFEditGuard
	}
	rows, err := tx.QueryContext(ctx, cfEditGuardFenceSQL, repoID)
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
			return errCFEditGuard
		}
		paths = append(paths, path)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
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

// Retained for the isolated Guard SQL tests. All production Branch update
// callers must prepare first and use cfV2EditCheckPreparedBranch.
func cfV2EditCheckUnscopedBranch(ctx context.Context, tx *sql.Tx, repoID, oldCommitID, newCommitID string) error {
	if !option.CheckoutEnabledForRepo(repoID) {
		return nil
	}
	if !option.V2GuardAvailable {
		return errCFEditGuard
	}
	changes, err := cfV2EditPrepareUnscopedBranch(repoID, oldCommitID, newCommitID)
	if err != nil {
		return err
	}
	return cfV2EditCheckPreparedBranch(ctx, tx, repoID, changes)
}

// Reviewed V2 fence query. Only this path-keyed store is supported; the
// resource-keyed 029 layout is archived by the reviewed SQL package and must
// not remain reachable as a runtime fallback.
const cfEditGuardFenceSQL = "SELECT path FROM cf_edit_guard FORCE INDEX (cf_edit_guard_active) " +
	"WHERE repo_id=? AND guard_id IS NOT NULL AND lease_expires_at>UTC_TIMESTAMP(6) FOR UPDATE"
