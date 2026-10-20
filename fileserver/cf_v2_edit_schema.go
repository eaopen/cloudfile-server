package main

// Validate the *installed* V2 Guard before accepting any file-server traffic.
// Older cf_edit_guard tables from the 029_editing_core migration use a
// resource_uid/lifecycle_ref/lease_until contract instead of the single V2
// (repo_id,path_hash)/path/lease_expires_at contract. A table-name probe alone
// would enable the final Branch barrier and break all ordinary uploads.
import (
	"context"
	"database/sql"
	"fmt"
	"strings"
)

type cfGuardColumn struct {
	kind     string
	length   int64
	nullable string
	unsigned bool
}
type cfGuardIndexPart struct {
	name      string
	nonUnique int64
	prefix    bool
}

var cfV2GuardColumns = map[string]cfGuardColumn{
	"repo_id":           {"char", 36, "NO", false},
	"path_hash":         {"binary", 32, "NO", false},
	"path":              {"varchar", 4096, "NO", false},
	"guard_id":          {"char", 36, "YES", false},
	"generation":        {"bigint", 0, "NO", true},
	"owner_uid":         {"varchar", 225, "YES", false},
	"owner_native_user": {"varchar", 255, "YES", false},
	"terminal_type":     {"varchar", 16, "YES", false},
	"terminal_name":     {"varchar", 128, "YES", false},
	"checkout_ip":       {"varchar", 45, "YES", false},
	"proof_digest":      {"char", 64, "YES", false},
	"base_file_id":      {"char", 40, "YES", false},
	"lease_expires_at":  {"datetime", 0, "YES", false},
	"created_at":        {"datetime", 0, "NO", false},
	"updated_at":        {"datetime", 0, "NO", false},
}
var cfV2GuardIndexes = map[string][]cfGuardIndexPart{
	"PRIMARY":             {{"repo_id", 0, false}, {"path_hash", 0, false}},
	"cf_edit_guard_id":    {{"guard_id", 0, false}},
	"cf_edit_guard_active": {{"repo_id", 1, false}, {"guard_id", 1, false}},
	"cf_edit_guard_owner": {{"owner_uid", 1, false}, {"guard_id", 1, false}},
}

// Keep the actual column/index contract in one place and independent of SQL
// diagnostics. No user/lease rows are read and no CREATE/ALTER is issued.
func cfV2GuardValidate(columns map[string]cfGuardColumn, indexes map[string][]cfGuardIndexPart) error {
	if len(columns) != len(cfV2GuardColumns) {
		return fmt.Errorf("expected %d V2 Guard columns, found %d", len(cfV2GuardColumns), len(columns))
	}
	for name, want := range cfV2GuardColumns {
		got, exists := columns[name]
		if !exists || got != want {
			return fmt.Errorf("Guard column %s is missing or incompatible", name)
		}
	}
	for name, want := range cfV2GuardIndexes {
		got := indexes[name]
		if len(got) != len(want) {
			return fmt.Errorf("Guard index %s is missing or incompatible", name)
		}
		for i := range want {
			if got[i] != want[i] {
				return fmt.Errorf("Guard index %s has incompatible key order or uniqueness", name)
			}
		}
	}
	return nil
}

func cfV2GuardRequireCurrent(ctx context.Context, db *sql.DB) error {
	rows, err := db.QueryContext(ctx,
		"SELECT COLUMN_NAME,DATA_TYPE,CHARACTER_MAXIMUM_LENGTH,IS_NULLABLE,COLUMN_TYPE "+
			"FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_guard'")
	if err != nil {
		return fmt.Errorf("cannot inspect Guard columns: %w", err)
	}
	columns := make(map[string]cfGuardColumn)
	for rows.Next() {
		var name, kind, nullable, fullKind string
		var length sql.NullInt64
		if err := rows.Scan(&name, &kind, &length, &nullable, &fullKind); err != nil {
			rows.Close()
			return fmt.Errorf("cannot read Guard column definition: %w", err)
		}
		columns[name] = cfGuardColumn{kind, length.Int64, nullable,
			strings.Contains(strings.ToLower(fullKind), "unsigned")}
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return fmt.Errorf("cannot finish Guard column inspection: %w", err)
	}

	rows, err = db.QueryContext(ctx,
		"SELECT INDEX_NAME,SEQ_IN_INDEX,COLUMN_NAME,NON_UNIQUE,SUB_PART "+
			"FROM information_schema.STATISTICS "+
			"WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_edit_guard' "+
			"ORDER BY INDEX_NAME,SEQ_IN_INDEX")
	if err != nil {
		return fmt.Errorf("cannot inspect Guard indexes: %w", err)
	}
	indexes := make(map[string][]cfGuardIndexPart)
	for rows.Next() {
		var key, name string
		var order, nonUnique int64
		var prefix sql.NullInt64
		if err := rows.Scan(&key, &order, &name, &nonUnique, &prefix); err != nil {
			rows.Close()
			return fmt.Errorf("cannot read Guard index definition: %w", err)
		}
		if order != int64(len(indexes[key])+1) {
			rows.Close()
			return fmt.Errorf("Guard index %s key order is invalid", key)
		}
		indexes[key] = append(indexes[key], cfGuardIndexPart{name, nonUnique, prefix.Valid})
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return fmt.Errorf("cannot finish Guard index inspection: %w", err)
	}
	return cfV2GuardValidate(columns, indexes)
}
