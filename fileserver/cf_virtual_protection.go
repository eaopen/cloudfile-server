package main

// Virtual publications serialize with protection changes on their origin.
// Resolve changed protected paths and immutable candidate objects before SQL locks.
import (
	"context"
	"database/sql"
	"strings"

	"github.com/haiwen/seafile-server/fileserver/commitmgr"
	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/fsmgr"
	"github.com/haiwen/seafile-server/fileserver/option"
)

type cfVirtualProtection struct {
	repo, origin, prefix string
	paths                []string
	candidate            map[string]string
}

type cfProtectionReader interface {
	QueryContext(context.Context, string, ...interface{}) (*sql.Rows, error)
}

func cfVirtualProtectedRows(ctx context.Context, db cfProtectionReader, origin, path string, locked bool) (map[string]string, error) {
	query := "SELECT path,file_id FROM cf_file_protection WHERE repo_id=? AND (path=? OR path LIKE ? ESCAPE '!') LIMIT 8193"
	if locked {
		query += " FOR UPDATE"
	}
	rows, err := db.QueryContext(ctx, query, origin, path, cfProtectionLikePattern(path))
	if err != nil {
		return nil, errCFProtectionUnavailable
	}
	defer rows.Close()
	result := make(map[string]string)
	for rows.Next() {
		var p, id string
		if rows.Scan(&p, &id) != nil || len(result) >= 8192 {
			return nil, errCFProtectionUnavailable
		}
		result[p] = id
	}
	if rows.Err() != nil {
		return nil, errCFProtectionUnavailable
	}
	return result, nil
}

func cfPrepareVirtualProtection(ctx context.Context, repo, oldID, newID string) (*cfVirtualProtection, error) {
	if len(option.CloudFileProtectionRepos) == 0 {
		return nil, nil
	}
	v := &cfVirtualProtection{repo: repo, candidate: make(map[string]string)}
	err := seafileDB.QueryRowContext(ctx, "SELECT origin_repo,path FROM VirtualRepo WHERE repo_id=?", repo).Scan(&v.origin, &v.prefix)
	if err == sql.ErrNoRows {
		return nil, nil
	}
	if err != nil {
		return nil, errCFProtectionUnavailable
	}
	if !option.ProtectionEnabledForRepo(v.origin) {
		return nil, nil
	}
	if v.origin == repo || v.prefix == "" || !strings.HasPrefix(v.prefix, "/") || strings.Contains(v.prefix, "//") || strings.HasSuffix(v.prefix, "/") {
		return nil, errCFProtectionUnavailable
	}
	old, err := commitmgr.Load(repo, oldID)
	if err != nil {
		return nil, errCFProtectionUnavailable
	}
	next, err := commitmgr.Load(repo, newID)
	if err != nil {
		return nil, errCFProtectionUnavailable
	}
	var changes []*diff.DiffEntry
	if diff.DiffCommits(old, next, &changes, false) != nil || len(changes) > 8192 {
		return nil, errCFProtectionUnavailable
	}
	seen := make(map[string]bool)
	for _, change := range changes {
		for _, name := range []string{change.Name, change.NewName} {
			if name == "" {
				continue
			}
			p := v.prefix + "/" + strings.TrimPrefix(name, "/")
			if seen[p] {
				continue
			}
			seen[p] = true
			v.paths = append(v.paths, p)
			protected, err := cfVirtualProtectedRows(ctx, seafileDB, v.origin, p, false)
			if err != nil {
				return nil, err
			}
			for path := range protected {
				if _, ok := v.candidate[path]; ok {
					continue
				}
				if len(v.candidate) >= 8192 {
					return nil, errCFProtectionUnavailable
				}
				id, mode, err := fsmgr.GetObjIDByPath(v.origin, next.RootID, strings.TrimPrefix(path, v.prefix))
				// Missing/invalid objects cannot prove preservation of the original.
				if err != nil || !fsmgr.IsRegular(mode) {
					id = ""
				}
				v.candidate[path] = id
			}
		}
	}
	return v, nil
}

func (v *cfVirtualProtection) lockOrigin(ctx context.Context, tx *sql.Tx) error {
	if v == nil {
		return nil
	}
	var head, origin, path string
	if tx.QueryRowContext(ctx, "SELECT commit_id FROM Branch WHERE name='master' AND repo_id=? FOR UPDATE", v.origin).Scan(&head) != nil {
		return errCFProtectionUnavailable
	}
	// A path remap invalidates the candidate proof, never silently redirects it.
	if tx.QueryRowContext(ctx, "SELECT origin_repo,path FROM VirtualRepo WHERE repo_id=? FOR UPDATE", v.repo).Scan(&origin, &path) != nil || origin != v.origin || path != v.prefix {
		return errCFProtectionUnavailable
	}
	return nil
}

func (v *cfVirtualProtection) check(ctx context.Context, tx *sql.Tx) error {
	if v == nil {
		return nil
	}
	for _, path := range v.paths {
		rows, err := cfVirtualProtectedRows(ctx, tx, v.origin, path, true)
		if err != nil {
			return err
		}
		for p, id := range rows {
			// Also allows native origin-to-view refresh preserving the protected ID.
			if v.candidate[p] == "" || v.candidate[p] != id {
				return errCFFileProtected
			}
		}
	}
	return nil
}
