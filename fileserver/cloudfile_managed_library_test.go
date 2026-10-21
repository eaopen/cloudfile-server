package main

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"errors"
	"io"
	"strings"
	"testing"
)

const scopedQuery = "SELECT repo_id FROM cf_dir_acl WHERE repo_id=? LIMIT 1 FOR UPDATE"

type scopedConnector struct { hasRule, fails bool }
type scopedDriver struct{}
type scopedConn struct { hasRule, fails bool }
type scopedTxn struct{}
type scopedRows struct { id string; yielded bool }

func (c scopedConnector) Driver() driver.Driver { return scopedDriver{} }
func (c scopedConnector) Connect(context.Context) (driver.Conn, error) {
	return &scopedConn{hasRule: c.hasRule, fails: c.fails}, nil
}
func (scopedDriver) Open(string) (driver.Conn, error) { return nil, errors.New("connector only") }
func (*scopedConn) Prepare(string) (driver.Stmt, error) { return nil, errors.New("no prepare") }
func (*scopedConn) Close() error { return nil }
func (*scopedConn) Begin() (driver.Tx, error) { return scopedTxn{}, nil }
func (c *scopedConn) BeginTx(context.Context, driver.TxOptions) (driver.Tx, error) { return scopedTxn{}, nil }
func (c *scopedConn) QueryContext(_ context.Context, query string, _ []driver.NamedValue) (driver.Rows, error) {
	if c.fails { return nil, errors.New("V2 ACL unavailable") }
	if strings.Contains(query, "FROM VirtualRepo") { return &scopedRows{}, nil }
	if query != scopedQuery { return nil, errors.New("unexpected SQL query") }
	if !c.hasRule { return &scopedRows{}, nil }
	return &scopedRows{id: "22222222-2222-2222-2222-222222222222"}, nil
}
func (scopedTxn) Commit() error { return nil }
func (scopedTxn) Rollback() error { return nil }
func (r *scopedRows) Columns() []string { return []string{"repo_id"} }
func (r *scopedRows) Close() error { return nil }
func (r *scopedRows) Next(dst []driver.Value) error {
	if r.id == "" || r.yielded { return io.EOF }
	dst[0] = r.id
	r.yielded = true
	return nil
}

func TestV2ScopedLibraryBoundary(t *testing.T) {
	for _, tc := range []struct { rule, fails, deny bool }{
		{false, false, false}, {true, false, true}, {false, true, true},
	} {
		db := sql.OpenDB(scopedConnector{hasRule:tc.rule, fails:tc.fails})
		tx, err := db.BeginTx(context.Background(), nil)
		if err != nil { t.Fatal(err) }
		err = checkLegacyLibrary(context.Background(), tx, "22222222-2222-2222-2222-222222222222")
		if (err != nil) != tc.deny { t.Fatalf("rule=%v fails=%v err=%v", tc.rule,tc.fails,err) }
		_ = tx.Rollback()
		_ = db.Close()
	}
}
