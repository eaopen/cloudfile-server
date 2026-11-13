package main

import (
	"context"
	"database/sql"
	"errors"
	"github.com/haiwen/seafile-server/fileserver/option"
	"strings"
	"testing"
)

func TestVirtualProtectionCandidateAndNewReservation(t *testing.T) {
	id := strings.Repeat("a", 40)
	for _, tc := range []struct {
		name, candidate string
		fail            bool
		want            error
	}{
		{"preserved original including origin refresh", id, false, nil},
		{"overwrite", strings.Repeat("b", 40), false, errCFFileProtected},
		{"new protection after preparation", "", false, errCFFileProtected},
		{"metadata unavailable", id, true, errCFProtectionUnavailable},
	} {
		t.Run(tc.name, func(t *testing.T) {
			f := &uploadGuardFixture{protectedRows: map[string]string{"/design/original": id}, failGuard: tc.fail, virtualPrefix: "/design"}
			db := sql.OpenDB(uploadGuardConnector{f})
			defer db.Close()
			tx, _ := db.Begin()
			defer tx.Rollback()
			v := &cfVirtualProtection{repo: "virtual", origin: "repo", prefix: "/design", paths: []string{"/design/original"}, candidate: map[string]string{"/design/original": tc.candidate}}
			if err := v.lockOrigin(context.Background(), tx); err != nil {
				t.Fatal(err)
			}
			if len(f.queries) != 2 || !strings.Contains(f.queries[0], "FROM Branch") || !strings.Contains(f.queries[1], "FROM VirtualRepo") {
				t.Fatal(f.queries)
			}
			if err := v.check(context.Background(), tx); !errors.Is(err, tc.want) {
				t.Fatalf("got %v want %v", err, tc.want)
			}
			f.virtualPrefix = "/moved"
			if err := v.lockOrigin(context.Background(), tx); !errors.Is(err, errCFProtectionUnavailable) {
				t.Fatalf("remap accepted: %v", err)
			}
		})
	}
}
func TestVirtualProtectionDisabledDoesNotQuery(t *testing.T) {
	old := option.CloudFileProtectionRepos
	option.CloudFileProtectionRepos = nil
	defer func() { option.CloudFileProtectionRepos = old }()
	v, err := cfPrepareVirtualProtection(context.Background(), "repo", "old", "new")
	if v != nil || err != nil {
		t.Fatalf("disabled queried metadata: %v", err)
	}
}
