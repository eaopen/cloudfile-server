package main

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"testing"
	"time"

	"github.com/haiwen/seafile-server/fileserver/option"
)

// Only an isolated test MariaDB may be used; no pre-existing database is opened.
func TestM1NativeBoundaryMariaDB(t *testing.T) {
	port := os.Getenv("CF_TEST_DB_PORT")
	if port == "" {
		t.Skip("requires isolated MariaDB")
	}
	admin, err := sql.Open("mysql", "root@tcp(127.0.0.1:"+port+")/")
	if err != nil {
		t.Fatal(err)
	}
	defer admin.Close()
	name := fmt.Sprintf("cf_m1_guard_%d", time.Now().UnixNano())
	if _, err = admin.Exec("CREATE DATABASE " + name); err != nil {
		t.Fatal(err)
	}
	defer admin.Exec("DROP DATABASE " + name)
	db, err := sql.Open("mysql", "root@tcp(127.0.0.1:"+port+")/"+name)
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	for _, statement := range []string{
		"CREATE TABLE cf_schema_migration(version VARCHAR(64) PRIMARY KEY,state VARCHAR(16)) ENGINE=InnoDB",
		"CREATE TABLE cf_managed_library(repo_id CHAR(36) CHARACTER SET ascii COLLATE ascii_bin PRIMARY KEY,created_at DATETIME(6) NOT NULL) ENGINE=InnoDB",
		"CREATE TABLE VirtualRepo(repo_id CHAR(36) PRIMARY KEY,origin_repo CHAR(36)) ENGINE=InnoDB",
	} {
		if _, err = db.Exec(statement); err != nil {
			t.Fatal(err)
		}
	}
	original := option.CloudFileManagedLibraryGuard
	option.CloudFileManagedLibraryGuard = true
	defer func() { option.CloudFileManagedLibraryGuard = original }()
	repo := "11111111-1111-4111-8111-111111111111"
	check := func(allowed bool) {
		t.Helper()
		tx, err := db.BeginTx(context.Background(), &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
		if err != nil {
			t.Fatal(err)
		}
		err = checkLegacyLibrary(context.Background(), tx, repo)
		tx.Rollback()
		if (err == nil) != allowed {
			t.Fatalf("native boundary allowed=%v, expected=%v: %v", err == nil, allowed, err)
		}
	}
	check(false) // A table without a verified profile receipt is unavailable.
	if _, err = db.Exec("INSERT INTO cf_schema_migration VALUES('101_m1_native_boundary','applied')"); err != nil {
		t.Fatal(err)
	}
	check(true) // Ordinary CE-only libraries retain their native behavior.
	if _, err = db.Exec("INSERT INTO cf_managed_library VALUES(?,NOW(6))", repo); err != nil {
		t.Fatal(err)
	}
	check(false)
	if _, err = db.Exec("DELETE FROM cf_managed_library"); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec("UPDATE cf_schema_migration SET state='failed'"); err != nil {
		t.Fatal(err)
	}
	check(false)
	if _, err = db.Exec("UPDATE cf_schema_migration SET state='applied'"); err != nil {
		t.Fatal(err)
	}
	if _, err = db.Exec("ALTER TABLE cf_managed_library DROP PRIMARY KEY"); err != nil {
		t.Fatal(err)
	}
	check(false)
}
