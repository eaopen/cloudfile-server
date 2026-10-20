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
	if strings.Contains(query, "information_schema.COLUMNS") {
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

var resourceGuardColumns = strings.Fields("resource_uid repo_id lifecycle_ref guard_id generation mode owner owner_native_user source holder credential_epoch proof_digest base_file_id pending_intent lease_until hard_expire_at")
var pathGuardColumns = strings.Fields("repo_id path_hash path guard_id generation owner_uid owner_native_user terminal_type terminal_name checkout_ip proof_digest base_file_id lease_expires_at created_at updated_at")

func TestUploadGuardAcceptsInstalledLayoutsWithoutDisablingTheFence(t *testing.T) {
	old := option.V2GuardAvailable
	option.V2GuardAvailable = true
	defer func() { option.V2GuardAvailable = old }()
	cases := []struct {
		name    string
		columns []string
		marker  string
	}{
		{"resource Checkout", resourceGuardColumns, "LEFT JOIN cf_resource"},
		{"path Checkout", pathGuardColumns, "FORCE INDEX (cf_edit_guard_active)"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			f := &uploadGuardFixture{columns: tc.columns}
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
			if len(f.queries) != 2 || !strings.Contains(f.queries[1], tc.marker) || !strings.Contains(f.queries[1], "FOR UPDATE") {
				t.Fatalf("wrong final guard query: %v", f.queries)
			}
			if tc.name == "resource Checkout" && (strings.Contains(f.queries[1], "lease_until>") || !strings.Contains(f.queries[1], "ELSE NULL")) {
				t.Fatalf("stale or orphan fence can disappear: %s", f.queries[1])
			}
		})
	}
}

func TestUploadGuardRefusesUnknownAmbiguousOrUnavailableState(t *testing.T) {
	old := option.V2GuardAvailable
	option.V2GuardAvailable = true
	defer func() { option.V2GuardAvailable = old }()
	for _, f := range []*uploadGuardFixture{
		{columns: []string{"repo_id", "guard_id"}},
		// The previously accepted five-column subset is not the installed DDL.
		{columns: []string{"repo_id", "path_hash", "path", "guard_id", "lease_expires_at"}},
		{columns: append(append([]string{}, pathGuardColumns...), "resource_uid")},
		{columns: append(append([]string{}, resourceGuardColumns...), "path")},
		{columns: append(append([]string{}, resourceGuardColumns...), pathGuardColumns...)},
		{columns: resourceGuardColumns, failMetadata: true},
		{columns: resourceGuardColumns, failGuard: true},
		{columns: resourceGuardColumns, paths: []driver.Value{nil}},
		{columns: resourceGuardColumns, paths: []driver.Value{"relative/path"}},
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
