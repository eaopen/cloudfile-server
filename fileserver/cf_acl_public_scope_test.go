package main

import (
	"context"
	"database/sql"
	"database/sql/driver"
	"errors"
	"io"
	"net/http"
	"strings"
	"testing"
)

type linkConnector struct { matched, failed bool }
type linkDriver struct{}
type linkConn struct { linkConnector }
type linkTx struct{}
type linkRows struct { matched, read bool }

func (c linkConnector) Connect(context.Context) (driver.Conn,error) { return &linkConn{c},nil }
func (c linkConnector) Driver() driver.Driver { return linkDriver{} }
func (linkDriver) Open(string) (driver.Conn,error) { return nil,errors.New("connector only") }
func (*linkConn) Prepare(string) (driver.Stmt,error) { return nil,errors.New("not used") }
func (*linkConn) Close() error { return nil }
func (*linkConn) Begin() (driver.Tx,error) { return linkTx{},nil }
func (linkTx) Commit() error { return nil }
func (linkTx) Rollback() error { return nil }
func (c *linkConn) QueryContext(_ context.Context, query string, _ []driver.NamedValue) (driver.Rows,error) {
	if c.failed { return nil,errors.New("ACL SQL unavailable") }
	if !strings.Contains(query,"FROM cf_dir_acl") || !strings.Contains(query,"path=? OR ? LIKE CONCAT(path,'/%')") { return nil,errors.New("wrong path-scoped query") }
	return &linkRows{matched:c.matched},nil
}
func (*linkRows) Columns() []string { return []string{"restricted"} }
func (*linkRows) Close() error { return nil }
func (r *linkRows) Next(dst []driver.Value) error {
	if r.read { return io.EOF }
	r.read=true
	dst[0]=r.matched
	return nil
}

func TestPublicShareOnlyChecksActualSubtree(t *testing.T) {
	previous:=seafileDB
	defer func(){ seafileDB=previous }()
	const repo="11111111-1111-4111-8111-111111111111"
	for _,tc:=range []struct { matched, failed bool; status int }{
		{false,false,0},
		{true,false,http.StatusForbidden},
		{false,true,http.StatusServiceUnavailable},
	} {
		db:=sql.OpenDB(linkConnector{matched:tc.matched,failed:tc.failed})
		seafileDB=db
		result:=cfACLPublicLinkScope(repo,"/team/shared",true)
		if tc.status==0 && result!=nil { t.Fatalf("unrelated ACL blocked scoped link: %v",result) }
		if tc.status!=0 && (result==nil || result.Code!=tc.status) { t.Fatalf("expected %d got %v",tc.status,result) }
		_ = db.Close()
	}
}

func TestPublicShareRequiresCanonicalPath(t *testing.T) {
	if cfACLPublicLinkScope("not-a-repo","/team",false)==nil { t.Fatal("invalid repo accepted") }
	if cfACLPublicLinkScope("11111111-1111-4111-8111-111111111111","/../hidden",false)==nil { t.Fatal("invalid path accepted") }
}
