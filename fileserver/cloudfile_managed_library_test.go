package main

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"errors"
	"github.com/haiwen/seafile-server/fileserver/option"
	"strings"
	"testing"
	"time"
)

// Publication orchestration, not evidence of MySQL locking semantics.
type managedLibraryConnector struct {
	fixture *readAuditSQLFixture
	missing bool
}

func (c managedLibraryConnector) Driver() driver.Driver { return readAuditDriver{} }
func (c managedLibraryConnector) Connect(context.Context) (driver.Conn, error) {
	return &managedLibraryConnection{readAuditConnection: readAuditConnection{c.fixture}, missing: c.missing}, nil
}

type managedLibraryConnection struct {
	readAuditConnection
	missing bool
}

func (c *managedLibraryConnection) QueryContext(ctx context.Context, query string, args []driver.NamedValue) (driver.Rows, error) {
	if c.missing {
		return nil, errors.New("missing managed storage")
	}
	switch {
	case query == "SELECT @@transaction_isolation":
		return nil, errors.New("unknown variable")
	case query == "SELECT @@tx_isolation":
		return &readAuditRows{columns: []string{"isolation"}, values: [][]driver.Value{{"REPEATABLE-READ"}}}, nil
	case strings.Contains(query, "LIMIT 0"), strings.Contains(query, "FROM VirtualRepo"):
		return &readAuditRows{columns: []string{"repo_id"}}, nil
	case strings.Contains(query, "WHERE repo_id=? FOR UPDATE"):
		return &readAuditRows{columns: []string{"repo_id"}, values: [][]driver.Value{{args[0].Value}}}, nil
	default:
		return &readAuditRows{columns: []string{"count"}, values: [][]driver.Value{{int64(1)}}}, nil
	}
}
func TestLegacyManagedLibraryCannotPublishBranch(t *testing.T) {
	originalDB, originalGuard, originalTimeout := seafileDB, option.CloudFileManagedLibraryGuard, option.DBOpTimeout
	defer func() {
		seafileDB = originalDB
		option.CloudFileManagedLibraryGuard = originalGuard
		option.DBOpTimeout = originalTimeout
	}()
	option.CloudFileManagedLibraryGuard = true
	option.DBOpTimeout = 2 * time.Second
	for _, missing := range []bool{false, true} {
		fixture := &readAuditSQLFixture{}
		seafileDB = sql.OpenDB(managedLibraryConnector{fixture: fixture, missing: missing})
		_, err := updateBranch("22222222-2222-2222-2222-222222222222", "", strings.Repeat("a", 40), strings.Repeat("b", 40), "", false, "")
		seafileDB.Close()
		if err == nil || fixture.commits != 0 || fixture.rollbacks != 1 || len(fixture.execQueries) != 0 {
			t.Fatalf("protected/missing library published: err=%v fixture=%+v", err, fixture)
		}
	}
}

func TestLegacyManagedReadCompatibilityDoesNotPermitPublication(t *testing.T) {
	originalDB, originalGuard, originalReads, originalTimeout := seafileDB, option.CloudFileManagedLibraryGuard,
		option.CloudFileAllowLegacyManagedReads, option.DBOpTimeout
	defer func() {
		seafileDB = originalDB
		option.CloudFileManagedLibraryGuard = originalGuard
		option.CloudFileAllowLegacyManagedReads = originalReads
		option.DBOpTimeout = originalTimeout
	}()
	option.CloudFileManagedLibraryGuard = true
	option.CloudFileAllowLegacyManagedReads = true
	option.DBOpTimeout = 2 * time.Second
	fixture := &readAuditSQLFixture{}
	seafileDB = sql.OpenDB(managedLibraryConnector{fixture: fixture, missing: true})
	defer seafileDB.Close()
	if err := legacyLibraryAllowed(context.Background(), "22222222-2222-2222-2222-222222222222"); err != nil {
		t.Fatalf("native read compatibility was rejected: %v", err)
	}
	_, err := updateBranch("22222222-2222-2222-2222-222222222222", "", strings.Repeat("a", 40), strings.Repeat("b", 40), "", false, "")
	if !errors.Is(err, errCloudFileLegacyLibrary) || fixture.commits != 0 ||
		fixture.rollbacks != 1 || len(fixture.execQueries) != 0 {
		t.Fatalf("native mutation bypassed managed-library publication guard: err=%v fixture=%+v", err, fixture)
	}
}
