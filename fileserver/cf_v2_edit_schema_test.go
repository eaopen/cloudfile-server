package main

// Contract tests for the startup Guard validation added on top of the opt-in
// Checkout model. They exercise metadata-only validation, so no database is
// required; the failure cases mirror the legacy 029 layout and index drift
// that previously surfaced as SQL 1176 retries.

import (
	"testing"
)

func guardLayout() (map[string]cfGuardColumn, map[string][]cfGuardIndexPart) {
	columns := make(map[string]cfGuardColumn)
	for name, column := range cfV2GuardColumns {
		columns[name] = column
	}
	indexes := make(map[string][]cfGuardIndexPart)
	for name, parts := range cfV2GuardIndexes {
		indexes[name] = append([]cfGuardIndexPart(nil), parts...)
	}
	return columns, indexes
}

func TestCFV2GuardSchemaContract(t *testing.T) {
	columns, indexes := guardLayout()
	if err := cfV2GuardValidate(columns, indexes); err != nil {
		t.Fatalf("current V2 Guard rejected: %v", err)
	}
	for _, tc := range []struct {
		name string
		edit func(map[string]cfGuardColumn, map[string][]cfGuardIndexPart)
	}{
		{"old lifecycle_ref instead of path", func(c map[string]cfGuardColumn, _ map[string][]cfGuardIndexPart) {
			delete(c, "path")
			c["lifecycle_ref"] = cfGuardColumn{kind: "varchar", length: 512, nullable: "NO"}
		}},
		{"old lease_until instead of lease_expires_at", func(c map[string]cfGuardColumn, _ map[string][]cfGuardIndexPart) {
			delete(c, "lease_expires_at")
			c["lease_until"] = cfGuardColumn{kind: "datetime", nullable: "YES"}
		}},
		{"path hash stored as text", func(c map[string]cfGuardColumn, _ map[string][]cfGuardIndexPart) {
			c["path_hash"] = cfGuardColumn{kind: "char", length: 64, nullable: "NO"}
		}},
		{"missing active index", func(_ map[string]cfGuardColumn, idx map[string][]cfGuardIndexPart) {
			delete(idx, "cf_edit_guard_active")
		}},
		{"reversed active index", func(_ map[string]cfGuardColumn, idx map[string][]cfGuardIndexPart) {
			idx["cf_edit_guard_active"][0], idx["cf_edit_guard_active"][1] =
				idx["cf_edit_guard_active"][1], idx["cf_edit_guard_active"][0]
		}},
		{"old resource key", func(_ map[string]cfGuardColumn, idx map[string][]cfGuardIndexPart) {
			idx["PRIMARY"] = []cfGuardIndexPart{{name: "resource_uid", nonUnique: 0}}
		}},
	} {
		t.Run(tc.name, func(t *testing.T) {
			c, i := guardLayout()
			tc.edit(c, i)
			if err := cfV2GuardValidate(c, i); err == nil {
				t.Fatal("outdated or incompatible Guard layout passed")
			}
		})
	}
}
