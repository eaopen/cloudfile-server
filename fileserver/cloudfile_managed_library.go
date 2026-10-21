package main

import (
	"context"
	"database/sql"
	"errors"
	"io"
	"net/http"
	"sort"
	"time"

	"github.com/haiwen/seafile-server/fileserver/option"
)

var errCloudFileLegacyLibrary = errors.New("legacy library access unavailable")

// V2 scoped-library boundary. The legacy whole-repository native path may not
// bypass directory ACLs: a repository with scoped rules must use V2-authorized
// file endpoints. No legacy schema ledger or managed-library marker is needed.
// This indexed locking read also protects an empty prefix against a concurrent
// ACL insertion during native publication on InnoDB REPEATABLE READ.
func checkLegacyLibrary(ctx context.Context, tx *sql.Tx, repoID string) error {
	if tx == nil || len(repoID) != 36 {
		return errCloudFileLegacyLibrary
	}
	// Native virtual repositories may refer to protected origin paths.
	// Check both repository IDs under one transaction, in stable order.
	ids := []string{repoID}
	var origin sql.NullString
	err := tx.QueryRowContext(ctx,
		"SELECT origin_repo FROM VirtualRepo WHERE repo_id=? FOR UPDATE",
		repoID).Scan(&origin)
	if err != nil && err != sql.ErrNoRows {
		return errCloudFileLegacyLibrary
	}
	if err == nil {
		if !origin.Valid || len(origin.String) != 36 {
			return errCloudFileLegacyLibrary
		}
		if origin.String != repoID {
			ids = append(ids, origin.String)
		}
	}
	sort.Strings(ids)
	for _, id := range ids {
		var matched string
		err := tx.QueryRowContext(ctx,
			"SELECT repo_id FROM cf_dir_acl WHERE repo_id=? LIMIT 1 FOR UPDATE",
			id).Scan(&matched)
		if err == sql.ErrNoRows {
			continue
		}
		// Any directory ACL prevents unscoped native operations. A DB
		// failure must never be mistaken for an unprotected repository.
		return errCloudFileLegacyLibrary
	}
	return nil
}

func legacyLibraryAllowed(ctx context.Context, repoID string) error {
	// This read-only compatibility path keeps existing native download links
	// usable while the eTech delegated transfer client is being integrated.
	// Mutation publication still calls checkLegacyLibrary directly.
	if option.CloudFileAllowLegacyManagedReads {
		return nil
	}
	if seafileDB == nil {
		return errCloudFileLegacyLibrary
	}
	deadline, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	tx, err := seafileDB.BeginTx(deadline, &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
	if err != nil {
		return errCloudFileLegacyLibrary
	}
	defer tx.Rollback()
	return checkLegacyLibrary(deadline, tx, repoID)
}

// Existing enhanced transfers use their separately verified native guard.
// All ordinary file bytes/Range pass this current marker check, including
// share links and an access response obtained before library enrollment.
type cloudFileLegacyResponse struct {
	http.ResponseWriter
	request *http.Request
	repoID  string
}

func (w *cloudFileLegacyResponse) WriteHeader(status int) {
	if legacyLibraryAllowed(w.request.Context(), w.repoID) != nil {
		panic(http.ErrAbortHandler)
	}
	w.ResponseWriter.WriteHeader(status)
}

func (w *cloudFileLegacyResponse) Write(data []byte) (int, error) {
	total := 0
	for len(data) != 0 {
		if legacyLibraryAllowed(w.request.Context(), w.repoID) != nil {
			panic(http.ErrAbortHandler)
		}
		size := len(data)
		if size > cloudFileReadBlock {
			size = cloudFileReadBlock
		}
		count, err := w.ResponseWriter.Write(data[:size])
		total += count
		if err != nil {
			return total, err
		}
		if count != size {
			return total, io.ErrShortWrite
		}
		data = data[size:]
	}
	return total, nil
}
