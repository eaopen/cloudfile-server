package main

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"errors"
	"io"
	"strings"
	"testing"

	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/option"
)

// Exercise production SQL selection and row validation through database/sql.
// This fixture does not certify MySQL lock behavior or a deployed upload.
type uploadGuardFixture struct {
	protectedRows           map[string]string
	virtualPrefix           string
	columns                 []string
	paths                   []driver.Value
	failMetadata, failGuard bool
	tableCount              int
	currentSchema           bool
	queries                 []string
	protectionExact         bool
	protectionDescendant    bool
}
type uploadGuardDriver struct{}

func (uploadGuardDriver) Open(string) (driver.Conn, error) { return nil, errors.New("use connector") }

type uploadGuardConnector struct{ fixture *uploadGuardFixture }

func (c uploadGuardConnector) Driver() driver.Driver { return uploadGuardDriver{} }
func (c uploadGuardConnector) Connect(context.Context) (driver.Conn, error) {
	return &uploadGuardConn{c.fixture}, nil
}

type uploadGuardConn struct{ fixture *uploadGuardFixture }

func (c *uploadGuardConn) Prepare(string) (driver.Stmt, error) { return nil, errors.New("no prepare") }
func (c *uploadGuardConn) Close() error                        { return nil }
func (c *uploadGuardConn) Begin() (driver.Tx, error)           { return uploadGuardTx{}, nil }
func (c *uploadGuardConn) QueryContext(_ context.Context, query string, args []driver.NamedValue) (driver.Rows, error) {
	c.fixture.queries = append(c.fixture.queries, query)
	rows := &uploadGuardRows{names: []string{"value"}}
	if strings.Contains(query, "FROM cf_repo_feature") {
		rid := args[0].Value.(string)
		if _, ok := option.CloudFileCheckoutRepos[rid]; ok {
			rows.values = append(rows.values, []driver.Value{"checkout"})
		}
		if _, ok := option.CloudFileProtectionRepos[rid]; ok {
			rows.values = append(rows.values, []driver.Value{"protection"})
		}
	} else if strings.Contains(query, "information_schema.TABLES") {
		if c.fixture.failMetadata {
			return nil, errors.New("database unavailable")
		}
		rows.values = append(rows.values, []driver.Value{int64(c.fixture.tableCount)})
	} else if strings.Contains(query, "information_schema.COLUMNS") {
		if c.fixture.failMetadata {
			return nil, errors.New("database unavailable")
		}
		if c.fixture.currentSchema {
			rows.names = []string{"name", "kind", "length", "nullable", "full_kind"}
			for name, column := range cfV2GuardColumns {
				fullKind := column.kind
				if column.unsigned {
					fullKind += " unsigned"
				}
				rows.values = append(rows.values, []driver.Value{name, column.kind, column.length, column.nullable, fullKind})
			}
			return rows, nil
		}
		for _, name := range c.fixture.columns {
			rows.values = append(rows.values, []driver.Value{name})
		}
	} else if strings.Contains(query, "information_schema.STATISTICS") {
		rows.names = []string{"index", "order", "column", "non_unique", "prefix"}
		if c.fixture.currentSchema {
			for name, parts := range cfV2GuardIndexes {
				for i, part := range parts {
					rows.values = append(rows.values, []driver.Value{name, int64(i + 1), part.name, part.nonUnique, nil})
				}
			}
		}
	} else if strings.Contains(query, "FROM VirtualRepo") {
		rows.names = []string{"origin", "path"}
		if c.fixture.virtualPrefix != "" {
			rows.values = append(rows.values, []driver.Value{"repo", c.fixture.virtualPrefix})
		}
	} else if strings.Contains(query, "FROM Branch") {
		rows.values = append(rows.values, []driver.Value{strings.Repeat("a", 40)})
	} else if strings.Contains(query, "FROM cf_file_protection") {
		if c.fixture.failGuard {
			return nil, errors.New("protection SQL unavailable")
		}
		if len(args) < 2 || args[0].Value != "repo" {
			return nil, errors.New("wrong protected repository")
		}
		if strings.Contains(query, "SELECT path,file_id") {
			rows.names = []string{"path", "file_id"}
			for p, id := range c.fixture.protectedRows {
				rows.values = append(rows.values, []driver.Value{p, id})
			}
			return rows, nil
		}
		if (strings.Contains(query, "path LIKE") && c.fixture.protectionDescendant) ||
			(strings.Contains(query, "path_hash") && c.fixture.protectionExact) {
			rows.values = append(rows.values, []driver.Value{int64(1)})
		}
	} else {
		if c.fixture.failGuard {
			return nil, errors.New("guard unavailable")
		}
		if len(args) != 1 || args[0].Value != "repo" {
			return nil, errors.New("wrong repository scope")
		}
		for _, path := range c.fixture.paths {
			rows.values = append(rows.values, []driver.Value{path})
		}
	}
	return rows, nil
}

type uploadGuardTx struct{}

func (uploadGuardTx) Commit() error   { return nil }
func (uploadGuardTx) Rollback() error { return nil }

type uploadGuardRows struct {
	values [][]driver.Value
	names  []string
}

func (r *uploadGuardRows) Columns() []string { return r.names }
func (r *uploadGuardRows) Close() error      { return nil }
func (r *uploadGuardRows) Next(dest []driver.Value) error {
	if len(r.values) == 0 {
		return io.EOF
	}
	copy(dest, r.values[0])
	r.values = r.values[1:]
	return nil
}

func TestUploadGuardUsesReviewedPathQueryOnly(t *testing.T) {
	old, oldEnabled, oldRepos := option.V2GuardAvailable, option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos
	option.V2GuardAvailable, option.CloudFileCheckoutEnabled = true, true
	// Guard checks apply only to explicitly selected OPTIONAL repositories.
	option.CloudFileCheckoutRepos = map[string]struct{}{"repo": {}}
	defer func() {
		option.V2GuardAvailable, option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos = old, oldEnabled, oldRepos
	}()
	f := &uploadGuardFixture{}
	db := sql.OpenDB(uploadGuardConnector{f})
	defer db.Close()
	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	if err := cfV2EditCheckPreparedBranch(context.Background(), tx, "repo", []*diff.DiffEntry{}); err != nil {
		t.Fatal(err)
	}
	// Startup already qualified the schema: the commit path issues exactly one
	// Guard query, with no information_schema read and no cf_resource join.
	if len(f.queries) != 2 || !strings.Contains(f.queries[0], "cf_repo_feature") {
		t.Fatalf("expected one guard query, got %v", f.queries)
	}
	query := f.queries[1]
	if strings.Contains(query, "information_schema") || strings.Contains(query, "cf_resource") {
		t.Fatalf("legacy or repeated metadata query on commit path: %s", query)
	}
	if !strings.Contains(query, "FORCE INDEX (cf_edit_guard_active)") || !strings.Contains(query, "lease_expires_at>") {
		t.Fatalf("guard query lost its reviewed shape: %s", query)
	}
}

func TestUploadGuardRefusesGuardFailures(t *testing.T) {
	old, oldEnabled, oldRepos := option.V2GuardAvailable, option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos
	option.V2GuardAvailable, option.CloudFileCheckoutEnabled = true, true
	option.CloudFileCheckoutRepos = map[string]struct{}{"repo": {}}
	defer func() {
		option.V2GuardAvailable, option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos = old, oldEnabled, oldRepos
	}()
	for _, f := range []*uploadGuardFixture{
		{failGuard: true},
		{paths: []driver.Value{nil}},
		{paths: []driver.Value{"relative/path"}},
	} {
		db := sql.OpenDB(uploadGuardConnector{f})
		tx, err := db.Begin()
		if err != nil {
			t.Fatal(err)
		}
		if !errors.Is(cfV2EditCheckPreparedBranch(context.Background(), tx, "repo", []*diff.DiffEntry{}), errCFEditGuard) {
			t.Fatalf("unavailable or malformed guard was bypassed: %+v", f)
		}
		tx.Rollback()
		db.Close()
	}
}

// The old global bool cannot authorize or disable a dynamic repository.
// OFF libraries read just one feature index; Checkout Guard is never scanned.
func TestUploadCheckoutDynamicIgnoresGlobalFlag(t *testing.T) {
	previousGlobal, previousGuard, previousRepos :=
		option.CloudFileCheckoutEnabled, option.V2GuardAvailable, option.CloudFileCheckoutRepos
	defer func() {
		option.CloudFileCheckoutEnabled, option.V2GuardAvailable, option.CloudFileCheckoutRepos =
			previousGlobal, previousGuard, previousRepos
	}()
	option.CloudFileCheckoutEnabled = false
	option.V2GuardAvailable = true
	option.CloudFileCheckoutRepos = map[string]struct{}{}
	f := &uploadGuardFixture{}
	db := sql.OpenDB(uploadGuardConnector{f})
	defer db.Close()
	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	if err := cfV2EditCheckPreparedBranch(context.Background(), tx, "repo", nil); err != nil {
		t.Fatalf("OFF repo changed behavior: %v", err)
	}
	if len(f.queries) != 1 || !strings.Contains(f.queries[0], "cf_repo_feature") {
		t.Fatalf("OFF repository unexpectedly scanned Guard: %v", f.queries)
	}
	// Enable via the database fixture, without any global restart/config flip.
	option.CloudFileCheckoutRepos["repo"] = struct{}{}
	f.queries = nil
	if err := cfV2EditCheckPreparedBranch(context.Background(), tx, "repo", []*diff.DiffEntry{}); err != nil {
		t.Fatalf("dynamic Checkout with a valid Guard was rejected: %v", err)
	}
	if len(f.queries) != 2 || !strings.Contains(f.queries[1], "cf_edit_guard") {
		t.Fatalf("dynamic Checkout skipped the Guard fence: %v", f.queries)
	}
	// A previously selected repo must fail closed if Guard is unavailable.
	option.V2GuardAvailable = false
	if err := cfV2EditCheckPreparedBranch(context.Background(), tx, "repo",
		[]*diff.DiffEntry{}); !errors.Is(err, errCFEditGuard) {
		t.Fatalf("missing Guard allowed dynamically selected Checkout: %v", err)
	}
}

func TestCheckoutGuardSchemaProbeIsNotGloballyDisabled(t *testing.T) {
	old := option.CloudFileCheckoutEnabled
	option.CloudFileCheckoutEnabled = false
	defer func() { option.CloudFileCheckoutEnabled = old }()
	db := sql.OpenDB(uploadGuardConnector{&uploadGuardFixture{tableCount: 1, currentSchema: true}})
	defer db.Close()
	if available, err := loadV2GuardCapability(context.Background(), db); err != nil || !available {
		t.Fatalf("guard schema was skipped by deprecated global flag: %v", err)
	}
}

func TestUploadEnabledCheckoutRequiresInstalledGuard(t *testing.T) {
	enabled, repos := option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos
	option.CloudFileCheckoutEnabled = true
	option.CloudFileCheckoutRepos = map[string]struct{}{"repo": {}}
	defer func() { option.CloudFileCheckoutEnabled, option.CloudFileCheckoutRepos = enabled, repos }()
	for _, f := range []*uploadGuardFixture{{tableCount: 1, currentSchema: true}, {tableCount: 1}, {tableCount: 0}, {failMetadata: true}} {
		db := sql.OpenDB(uploadGuardConnector{f})
		available, err := loadV2GuardCapability(context.Background(), db)
		if f.tableCount == 1 && f.currentSchema && !f.failMetadata {
			if err != nil || !available {
				t.Fatal("installed Guard not enabled")
			}
		} else if err == nil || available {
			t.Fatal("enabled Checkout accepted unavailable Guard")
		}
		db.Close()
	}
}

func usePolicyFixture(t *testing.T) {
	previous := seafileDB
	seafileDB = sql.OpenDB(uploadGuardConnector{&uploadGuardFixture{}})
	t.Cleanup(func() { seafileDB.Close(); seafileDB = previous })
}
