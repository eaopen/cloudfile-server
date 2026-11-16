package main

import (
	"context"
	"testing"

	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/option"
)

// These are guard-hit tests only. They do not certify MariaDB native publish.
func TestCFV2EditTouchesPath(t *testing.T) {
	cases := []struct {
		name, previous, renamed, guarded string
		hit                              bool
	}{
		{"same file", "Engineering/part.step", "", "/Engineering/part.step", true},
		{"other file", "Engineering/other.step", "", "/Engineering/part.step", false},
		{"parent directory move", "Engineering", "Engineering-old", "/Engineering/part.step", true},
		{"rename target", "old.step", "Engineering/part.step", "/Engineering/part.step", true},
		{"prefix sibling", "Engineering-old", "", "/Engineering/part.step", false},
		{"nested directory", "Engineering/sub", "", "/Engineering/sub/part.step", true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			e := &diff.DiffEntry{Name: tc.previous, NewName: tc.renamed}
			if got := cfV2EditTouchesPath(e, tc.guarded); got != tc.hit {
				t.Fatalf("touches=%v, expected=%v", got, tc.hit)
			}
		})
	}
}

// Even with Checkout enabled elsewhere, OFF repositories do not require a DB.
func TestCFV2OffRepoSkipsGuardSQL(t *testing.T) {
	oldAvailable, oldOn, oldRepos := option.V2GuardAvailable, option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos
	defer func() {
		option.V2GuardAvailable, option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos = oldAvailable, oldOn, oldRepos
	}()
	option.CloudFileCheckoutEnabled = true
	option.CloudFileCheckoutRepos = map[string]struct{}{"11111111-1111-4111-8111-111111111111": {}}
	option.V2GuardAvailable = false
	usePolicyFixture(t)
	tx, _ := seafileDB.Begin()
	defer tx.Rollback()
	if err := cfV2EditCheckPreparedBranch(context.Background(), tx, "22222222-2222-4222-8222-222222222222", nil); err != nil {
		t.Fatal(err)
	}
	if err := cfV2EditCheckPreparedBranch(context.Background(), tx, "11111111-1111-4111-8111-111111111111", []*diff.DiffEntry{}); err != errCFEditGuard {
		t.Fatal(err)
	}
}

func TestCFV2MissingPolicyTransactionRefusesPublish(t *testing.T) {
	if err := cfV2EditCheckPreparedBranch(context.Background(), nil, "repo", nil); err == nil {
		t.Fatal("missing policy read")
	}
}
