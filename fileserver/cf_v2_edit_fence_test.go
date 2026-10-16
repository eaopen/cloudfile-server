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

func TestCFV2EditDisabledNeedsNoDatabase(t *testing.T) {
	old := option.V2EditEnabled
	option.V2EditEnabled = false
	defer func() { option.V2EditEnabled = old }()
	if err := cfV2EditCheckUnscopedBranch(context.Background(), nil, "", "", ""); err != nil {
		t.Fatalf("disabled feature must make no database call: %v", err)
	}
}
