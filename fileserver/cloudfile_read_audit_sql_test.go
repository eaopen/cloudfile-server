package main

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"encoding/json"
	"errors"
	"io"
	"strings"
	"testing"
)

// Exercises the actual writer through database/sql, not a replacement writer.
// It proves orchestration only, not MySQL syntax, locking or durability.
type readAuditSQLFixture struct {
	execQueries                    []string
	execArgs                       [][]driver.NamedValue
	begins, commits, rollbacks     int
	failExec                       int
	commitFailure                  bool
	badEngine, badLedger, badShape bool
}
type readAuditConnector struct{ fixture *readAuditSQLFixture }
type readAuditDriver struct{}

func (readAuditDriver) Open(string) (driver.Conn, error) { return nil, errors.New("use connector") }
func (c readAuditConnector) Driver() driver.Driver       { return readAuditDriver{} }
func (c readAuditConnector) Connect(context.Context) (driver.Conn, error) {
	return &readAuditConnection{c.fixture}, nil
}

type readAuditConnection struct{ fixture *readAuditSQLFixture }

func (c *readAuditConnection) Close() error { return nil }
func (c *readAuditConnection) Prepare(string) (driver.Stmt, error) {
	return nil, errors.New("unexpected prepare")
}
func (c *readAuditConnection) Begin() (driver.Tx, error) {
	c.fixture.begins++
	return readAuditTransaction{c.fixture}, nil
}
func (c *readAuditConnection) BeginTx(ctx context.Context, _ driver.TxOptions) (driver.Tx, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	return c.Begin()
}
func (c *readAuditConnection) ExecContext(ctx context.Context, query string, args []driver.NamedValue) (driver.Result, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	c.fixture.execQueries = append(c.fixture.execQueries, query)
	c.fixture.execArgs = append(c.fixture.execArgs, append([]driver.NamedValue(nil), args...))
	if c.fixture.failExec == len(c.fixture.execQueries) {
		return nil, errors.New("fixture SQL failure")
	}
	return readAuditResult{}, nil
}
func (c *readAuditConnection) QueryContext(ctx context.Context, query string, _ []driver.NamedValue) (driver.Rows, error) {
	if err := ctx.Err(); err != nil {
		return nil, err
	}
	switch {
	case strings.Contains(query, "information_schema.tables"):
		value := int64(3)
		if c.fixture.badEngine {
			value = 2
		}
		return &readAuditRows{columns: []string{"count"}, values: [][]driver.Value{{value}}}, nil
	case strings.Contains(query, "SELECT version,step"):
		step := int64(16)
		if c.fixture.badLedger {
			step = 15
		}
		return &readAuditRows{columns: []string{"version", "step"}, values: [][]driver.Value{{"003_outbox", int64(1)}, {"004_audit", step}}}, nil
	case query == cloudFileReadAuditSchemaSQL:
		value := int64(1)
		if c.fixture.badShape {
			value = 0
		}
		return &readAuditRows{columns: []string{"valid"}, values: [][]driver.Value{{value}}}, nil
	default:
		return nil, errors.New("unexpected query")
	}
}

type readAuditTransaction struct{ fixture *readAuditSQLFixture }

func (t readAuditTransaction) Commit() error {
	t.fixture.commits++
	if t.fixture.commitFailure {
		return errors.New("uncertain commit")
	}
	return nil
}
func (t readAuditTransaction) Rollback() error { t.fixture.rollbacks++; return nil }

type readAuditRows struct {
	columns  []string
	values   [][]driver.Value
	position int
}

func (r *readAuditRows) Columns() []string { return r.columns }
func (r *readAuditRows) Close() error      { return nil }
func (r *readAuditRows) Next(dest []driver.Value) error {
	if r.position == len(r.values) {
		return io.EOF
	}
	copy(dest, r.values[r.position])
	r.position++
	return nil
}

// RowsAffected does not provide LastInsertId: use a result matching MySQL.
type readAuditResult struct{}

func (readAuditResult) LastInsertId() (int64, error) { return 1, nil }
func (readAuditResult) RowsAffected() (int64, error) { return 1, nil }

func sqlReadAuditFact() cloudFileReadAuditFact {
	return cloudFileReadAuditFact{RequestID: "11111111-1111-1111-1111-111111111111", RepoID: "22222222-2222-2222-2222-222222222222",
		UserID: "business-user", Path: "/file", HeadID: strings.Repeat("a", 40), Epoch: strings.Repeat("b", 32), Operation: "download",
		Outcome: cloudFileReadOutcome{Result: "stream_completed", Status: 200, BytesSent: 3}}
}

func TestReadAuditSQLFailuresNeverCommitPartialFact(t *testing.T) {
	for _, fixture := range []*readAuditSQLFixture{{failExec: 1}, {failExec: 2}, {failExec: 3}, {badEngine: true}, {badLedger: true}, {badShape: true}} {
		database := sql.OpenDB(readAuditConnector{fixture})
		err := appendCloudFileReadAudit(context.Background(), database, sqlReadAuditFact())
		database.Close()
		if err == nil || fixture.begins != 1 || fixture.commits != 0 || fixture.rollbacks != 1 {
			t.Fatalf("partial fact committed: %#v, %v", fixture, err)
		}
	}
}

func TestReadAuditSQLCommitUsesOneTransactionAndNeverRetries(t *testing.T) {
	for _, uncertain := range []bool{false, true} {
		fixture := &readAuditSQLFixture{commitFailure: uncertain}
		database := sql.OpenDB(readAuditConnector{fixture})
		err := appendCloudFileReadAudit(context.Background(), database, sqlReadAuditFact())
		database.Close()
		if (err != nil) != uncertain || fixture.begins != 1 || fixture.commits != 1 || len(fixture.execQueries) != 3 {
			t.Fatalf("unexpected commit/retry: %#v, %v", fixture, err)
		}
		var payload map[string]interface{}
		if json.Unmarshal([]byte(fixture.execArgs[1][0].Value.(string)), &payload) != nil || payload["actor_user_id"] != "business-user" || payload["bytes_sent"] != float64(3) {
			t.Fatal("server fact lost")
		}
		if payload["event_id"] != fixture.execArgs[0][0].Value || fixture.execArgs[2][6].Value != payload["event_id"] {
			t.Fatal("outbox and audit identity differ")
		}
	}
}
