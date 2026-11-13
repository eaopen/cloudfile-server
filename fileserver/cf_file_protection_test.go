package main

import (
	"context"
	"database/sql"
	"errors"
	"strings"
	"testing"

	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/option"
)

// Uses the same bounded database/sql fixture as the existing upload Guard
// tests. Not a replacement for native C/Go + real MariaDB integration.
func withProtectionFixture(t *testing.T, f *uploadGuardFixture, run func(*sql.Tx)) {
	t.Helper()
	oldProtection, oldCheckout, oldRepos := option.CloudFileProtectionRepos,
		option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos
	option.CloudFileProtectionRepos = map[string]struct{}{"repo": {}}
	option.CloudFileCheckoutEnabled = false
	option.CloudFileCheckoutRepos = map[string]struct{}{}
	defer func() {
		option.CloudFileProtectionRepos = oldProtection
		option.CloudFileCheckoutEnabled = oldCheckout
		option.CloudFileCheckoutRepos = oldRepos
	}()
	db := sql.OpenDB(uploadGuardConnector{f})
	defer db.Close()
	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	run(tx)
}

func TestFileProtectionOffRepoHasNoSQL(t *testing.T) {
	old := option.CloudFileProtectionRepos
	defer func() { option.CloudFileProtectionRepos = old }()
	option.CloudFileProtectionRepos = map[string]struct{}{}
	if err := cfProtectionChanges(context.Background(), nil, "repo", nil); err != nil {
		t.Fatalf("OFF repo required protection SQL: %v", err)
	}
}

func TestFileProtectionSelectedWithoutStoreFailsClosed(t *testing.T) {
	old := option.CloudFileProtectionRepos
	defer func() { option.CloudFileProtectionRepos = old }()
	option.CloudFileProtectionRepos = map[string]struct{}{"repo": {}}
	if err := cfProtectionChanges(context.Background(), nil, "repo",
		[]*diff.DiffEntry{{Name: "design.prt"}}); !errors.Is(err, errCFProtectionUnavailable) {
		t.Fatalf("selected repo without final transaction was allowed: %v", err)
	}
}

func TestFileProtectionExactAndParentDirectory(t *testing.T) {
	for _, tc := range []struct {
		name     string
		f        uploadGuardFixture
		change   string
		expected error
	}{
		{"protected original", uploadGuardFixture{protectionExact: true}, "design/original.prt", errCFFileProtected},
		{"parent rename or delete", uploadGuardFixture{protectionDescendant: true}, "design", errCFFileProtected},
		{"other file", uploadGuardFixture{}, "design/work.prt", nil},
		{"unavailable metadata", uploadGuardFixture{failGuard: true}, "design/original.prt", errCFProtectionUnavailable},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := tc.f
			withProtectionFixture(t, &f, func(tx *sql.Tx) {
				err := cfV2EditCheckPreparedBranch(context.Background(), tx, "repo",
					[]*diff.DiffEntry{{Name: tc.change}})
				if !errors.Is(err, tc.expected) {
					t.Fatalf("result=%v want=%v", err, tc.expected)
				}
			})
			if len(f.queries) < 1 || len(f.queries) > 2 {
				t.Fatalf("expected 1-2 scoped Protection SQL checks, got %d", len(f.queries))
			}
			for _, query := range f.queries {
				if !strings.Contains(query, "cf_file_protection") ||
					!strings.Contains(query, "repo_id=?") || !strings.Contains(query, "FOR UPDATE") {
					t.Fatalf("unsafe protection query: %s", query)
				}
			}
		})
	}
}

func TestFileProtectionCopyAndEscapedDirectories(t *testing.T) {
	// A source that is protected isn't a candidate write. A copy's candidate
	// diff contains only its destination and so creates an editable copy.
	f := &uploadGuardFixture{}
	withProtectionFixture(t, f, func(tx *sql.Tx) {
		if err := cfProtectionChanges(context.Background(), tx, "repo",
			[]*diff.DiffEntry{{Name: "design/work-copy.prt"}}); err != nil {
			t.Fatalf("copy to new unprotected path: %v", err)
		}
	})
	if pattern := cfProtectionLikePattern("/A_100%/x!"); pattern != "/A!_100!%/x!!/%" {
		t.Fatalf("LIKE wildcard escaping failed: %q", pattern)
	}
}
