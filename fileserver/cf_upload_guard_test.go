package main

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"errors"
	"io"
	"strings"
	"testing"

	"github.com/haiwen/seafile-server/fileserver/option"
)

// Exercise production SQL selection and row validation through database/sql.
// This fixture does not certify MySQL lock behavior or a deployed upload.
type uploadGuardFixture struct {
	columns                 []string
	paths                   []driver.Value
	failMetadata, failGuard bool
	tableCount              int
	queries                 []string
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
	rows := &uploadGuardRows{}
	if strings.Contains(query, "information_schema.TABLES") {
		if c.fixture.failMetadata {
			return nil, errors.New("database unavailable")
		}
		rows.values = append(rows.values, []driver.Value{int64(c.fixture.tableCount)})
	} else if strings.Contains(query, "information_schema.COLUMNS") {
		if c.fixture.failMetadata {
			return nil, errors.New("database unavailable")
		}
		for _, name := range c.fixture.columns {
			rows.values = append(rows.values, []driver.Value{name})
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

type uploadGuardRows struct{ values [][]driver.Value }

func (r *uploadGuardRows) Columns() []string { return []string{"value"} }
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
	old, oldEnabled := option.V2GuardAvailable, option.CloudFileCheckoutEnabled
	option.V2GuardAvailable, option.CloudFileCheckoutEnabled = true, true
	defer func() { option.V2GuardAvailable, option.CloudFileCheckoutEnabled = old, oldEnabled }()
	f := &uploadGuardFixture{}
	db := sql.OpenDB(uploadGuardConnector{f})
	defer db.Close()
	tx, err := db.Begin()
	if err != nil {
		t.Fatal(err)
	}
	defer tx.Rollback()
	if err := cfV2EditCheckUnscopedBranch(context.Background(), tx, "repo", "old", "new"); err != nil {
		t.Fatal(err)
	}
	// Startup already qualified the schema: the commit path issues exactly one
	// Guard query, with no information_schema read and no cf_resource join.
	if len(f.queries) != 1 {
		t.Fatalf("expected one guard query, got %v", f.queries)
	}
	query := f.queries[0]
	if strings.Contains(query, "information_schema") || strings.Contains(query, "cf_resource") {
		t.Fatalf("legacy or repeated metadata query on commit path: %s", query)
	}
	if !strings.Contains(query, "FORCE INDEX (cf_edit_guard_active)") || !strings.Contains(query, "lease_expires_at>") {
		t.Fatalf("guard query lost its reviewed shape: %s", query)
	}
}

func TestUploadGuardRefusesGuardFailures(t *testing.T) {
	old, oldEnabled := option.V2GuardAvailable, option.CloudFileCheckoutEnabled
	option.V2GuardAvailable, option.CloudFileCheckoutEnabled = true, true
	defer func() { option.V2GuardAvailable, option.CloudFileCheckoutEnabled = old, oldEnabled }()
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
		if !errors.Is(cfV2EditCheckUnscopedBranch(context.Background(), tx, "repo", "old", "new"), errCFEditGuard) {
			t.Fatalf("unavailable or malformed guard was bypassed: %+v", f)
		}
		tx.Rollback()
		db.Close()
	}
}

// Nil DB/transaction proves the disabled path performs no metadata or Guard SQL.
func TestUploadCheckoutDisabledNeverQueriesGuard(t *testing.T) {
	old, enabled := option.V2GuardAvailable, option.CloudFileCheckoutEnabled
	defer func() { option.V2GuardAvailable, option.CloudFileCheckoutEnabled = old, enabled }()
	option.CloudFileCheckoutEnabled, option.V2GuardAvailable = false, true
	if available, err := loadV2GuardCapability(context.Background(), nil); err != nil || available {
		t.Fatalf("disabled capability touched the database: %v", err)
	}
	if err := cfV2EditCheckUnscopedBranch(context.Background(), nil, "repo", "old", "new"); err != nil {
		t.Fatalf("disabled upload queried Guard: %v", err)
	}
	option.CloudFileCheckoutEnabled, option.V2GuardAvailable = true, false
	if !errors.Is(cfV2EditCheckUnscopedBranch(context.Background(), nil, "repo", "old", "new"), errCFEditGuard) {
		t.Fatal("enabled but unavailable Guard was bypassed")
	}
}

func TestUploadEnabledCheckoutRequiresInstalledGuard(t *testing.T) {
	enabled := option.CloudFileCheckoutEnabled
	option.CloudFileCheckoutEnabled = true
	defer func() { option.CloudFileCheckoutEnabled = enabled }()
	for _, f := range []*uploadGuardFixture{{tableCount: 1}, {tableCount: 0}, {failMetadata: true}} {
		db := sql.OpenDB(uploadGuardConnector{f})
		available, err := loadV2GuardCapability(context.Background(), db)
		if f.tableCount == 1 && !f.failMetadata {
			if err != nil || !available {
				t.Fatal("installed Guard not enabled")
			}
		} else if err == nil || available {
			t.Fatal("enabled Checkout accepted unavailable Guard")
		}
		db.Close()
	}
}
