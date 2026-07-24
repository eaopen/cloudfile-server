package objstore

import (
	"fmt"
	"os"
	"path"
	"strings"
	"testing"
)

const (
	testFile        = "output.data"
	seafileConfPath = "/tmp/conf"
	seafileDataDir  = "/tmp/conf/seafile-data"
	repoID          = "b1f2ad61-9164-418a-a47f-ab805dbd5694"
	objID           = "0401fc662e3bc87a41f299a907c056aaf8322a27"
)

func createFile() error {
	outputFile, err := os.OpenFile(testFile, os.O_WRONLY|os.O_CREATE, 0666)
	if err != nil {
		return err
	}
	defer outputFile.Close()

	outputString := "hello world!\n"
	for i := 0; i < 10; i++ {
		outputFile.WriteString(outputString)
	}

	return nil
}

func TestS3BackendConfig(t *testing.T) {
	conf, err := os.CreateTemp(t.TempDir(), "seafile.conf")
	if err != nil {
		t.Fatal(err)
	}
	_, err = conf.WriteString(`[commit_object_backend]
name = s3
bucket = commits
key_id = access
key = secret
host = minio:9000
use_https = false
use_v4_signature = true
path_style_request = true
`)
	if err != nil {
		t.Fatal(err)
	}
	if err = conf.Close(); err != nil {
		t.Fatal(err)
	}
	backend, err := newBackend(conf.Name(), t.TempDir(), "commits")
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := backend.(*s3Backend); !ok {
		t.Fatalf("backend = %T, want *s3Backend", backend)
	}
}

func TestS3BackendRejectsIncompleteConfig(t *testing.T) {
	conf, err := os.CreateTemp(t.TempDir(), "seafile.conf")
	if err != nil {
		t.Fatal(err)
	}
	_, err = conf.WriteString("[commit_object_backend]\nname = s3\n")
	if err != nil {
		t.Fatal(err)
	}
	if err = conf.Close(); err != nil {
		t.Fatal(err)
	}
	_, err = newBackend(conf.Name(), t.TempDir(), "commits")
	if err == nil || !strings.Contains(err.Error(), "is required") {
		t.Fatalf("error = %v, want missing S3 setting", err)
	}
}

func TestMultipleBackendConfig(t *testing.T) {
	dir := t.TempDir()
	classes := path.Join(dir, "classes.json")
	if err := os.WriteFile(classes, []byte(`[
  {"storage_id":"local","is_default":true,"commits":{"backend":"fs","dir":"`+dir+`"},"fs":{"backend":"fs","dir":"`+dir+`"},"blocks":{"backend":"fs","dir":"`+dir+`"}}
]`), 0600); err != nil {
		t.Fatal(err)
	}
	conf := path.Join(dir, "seafile.conf")
	if err := os.WriteFile(conf, []byte("[database]\nhost = db\nuser = user\npassword = pass\ndb_name = seafile_db\n\n[storage]\nenable_storage_classes = true\nstorage_classes_file = "+classes+"\n\n[commit_object_backend]\nname = multiple\n"), 0600); err != nil {
		t.Fatal(err)
	}
	backend, err := newBackend(conf, dir, "commits")
	if err != nil {
		t.Fatal(err)
	}
	multi, ok := backend.(*multiBackend)
	if !ok || multi.defaultID != "local" {
		t.Fatalf("backend = %#v, want local multi backend", backend)
	}
}

func delFile() error {
	err := os.Remove(testFile)
	if err != nil {
		return err
	}

	err = os.RemoveAll(seafileConfPath)
	if err != nil {
		return err
	}

	return nil
}

func TestMain(m *testing.M) {
	err := createFile()
	if err != nil {
		fmt.Printf("Failed to create test file : %v\n", err)
		os.Exit(1)
	}
	code := m.Run()
	err = delFile()
	if err != nil {
		fmt.Printf("Failed to remove test file : %v\n", err)
		os.Exit(1)
	}
	os.Exit(code)
}

func testWrite(t *testing.T) {
	inputFile, err := os.Open(testFile)
	if err != nil {
		t.Errorf("Failed to open test file : %v\n", err)
	}
	defer inputFile.Close()

	bend := New(seafileConfPath, seafileDataDir, "commit")
	bend.Write(repoID, objID, inputFile, true)
}

func testRead(t *testing.T) {
	outputFile, err := os.OpenFile(testFile, os.O_WRONLY, 0666)
	if err != nil {
		t.Errorf("Failed to open test file:%v\n", err)
	}
	defer outputFile.Close()

	bend := New(seafileConfPath, seafileDataDir, "commit")
	err = bend.Read(repoID, objID, outputFile)
	if err != nil {
		t.Errorf("Failed to read backend : %s\n", err)
	}
}

func testExists(t *testing.T) {
	bend := New(seafileConfPath, seafileDataDir, "commit")
	ret, _ := bend.Exists(repoID, objID)
	if !ret {
		t.Errorf("File is not exist\n")
	}

	filePath := path.Join(seafileDataDir, "storage", "commit", repoID, objID[:2], objID[2:])
	fileInfo, _ := os.Stat(filePath)
	if fileInfo.Size() != 130 {
		t.Errorf("File is exist, but the size of file is incorrect.\n")
	}
}

func TestObjStore(t *testing.T) {
	testWrite(t)
	testRead(t)
	testExists(t)
}
