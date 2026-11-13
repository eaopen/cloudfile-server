package main

// Independent permanent file protection. Unlike Checkout it has no owner,
// lease, renewal or publication bypass. Protect/unprotect is a Hub operation
// serialized with native Branch(master); native writes read in that transaction.
import (
	"context"
	"crypto/sha256"
	"database/sql"
	"errors"
	"strings"

	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/option"
)

var errCFFileProtected = errors.New("FILE_PROTECTED")
var errCFProtectionUnavailable = errors.New("FILE_PROTECTION_UNAVAILABLE")

const cfProtectionExactSQL = "SELECT 1 FROM cf_file_protection " +
	"WHERE repo_id=? AND path_hash=? AND path=? LIMIT 1 FOR UPDATE"
const cfProtectionDescendantSQL = "SELECT 1 FROM cf_file_protection FORCE INDEX (cf_file_protection_prefix) " +
	"WHERE repo_id=? AND path LIKE ? ESCAPE '!' LIMIT 1 FOR UPDATE"

// Escape using ! rather than MySQL backslash whose meaning depends on sql_mode.
func cfProtectionLikePattern(value string) string {
	return strings.NewReplacer("!", "!!", "%", "!%", "_", "!_").Replace(value) + "/%"
}

func cfProtectionPath(ctx context.Context, tx *sql.Tx, repoID, changed string) error {
	if tx == nil || changed == "" || strings.ContainsRune(changed, '\x00') {
		return errCFProtectionUnavailable
	}
	target := "/" + strings.TrimPrefix(changed, "/")
	if len(target) > 4096 || target == "/" || strings.Contains(target, "//") {
		return errCFProtectionUnavailable
	}
	hash := sha256.Sum256([]byte(target))
	var found int
	err := tx.QueryRowContext(ctx, cfProtectionExactSQL, repoID, hash[:], target).Scan(&found)
	if err == nil {
		return errCFFileProtected
	}
	if err != sql.ErrNoRows {
		return errCFProtectionUnavailable
	}
	err = tx.QueryRowContext(ctx, cfProtectionDescendantSQL, repoID, cfProtectionLikePattern(target)).Scan(&found)
	if err == nil {
		return errCFFileProtected
	}
	if err != sql.ErrNoRows {
		return errCFProtectionUnavailable
	}
	return nil
}

func cfProtectionChanges(ctx context.Context, tx *sql.Tx, repoID string, changes []*diff.DiffEntry) error {
	if !option.ProtectionEnabledForRepo(repoID) {
		return nil
	}
	if tx == nil || changes == nil || len(changes) > 8192 {
		return errCFProtectionUnavailable
	}
	for _, change := range changes {
		if change == nil {
			return errCFProtectionUnavailable
		}
		seen := make(map[string]struct{}, 2)
		for _, name := range []string{change.Name, change.NewName} {
			if name == "" {
				continue
			}
			if _, exists := seen[name]; exists {
				continue
			}
			seen[name] = struct{}{}
			if err := cfProtectionPath(ctx, tx, repoID, name); err != nil {
				return err
			}
		}
	}
	return nil
}
