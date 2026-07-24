package objstore

import (
	"bytes"
	"context"
	"database/sql"
	"fmt"
	"io"
	"os"
	"path"
	"strings"
	"testing"

	"github.com/minio/minio-go/v7"
)

const (
	testFile        = "output.data"
	seafileConfPath = "/tmp/conf"
	seafileDataDir  = "/tmp/conf/seafile-data"
	repoID          = "b1f2ad61-9164-418a-a47f-ab805dbd5694"
	objID           = "0401fc662e3bc87a41f299a907c056aaf8322a27"
)

type recordingBackend struct {
	writes int
}

func (b *recordingBackend) read(string, string, io.Writer) error { return nil }
func (b *recordingBackend) write(string, string, io.Reader, bool) error {
	b.writes++
	return nil
}
func (b *recordingBackend) exists(string, string) (bool, error) { return false, nil }
func (b *recordingBackend) stat(string, string) (int64, error)  { return 0, nil }

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

func TestMultipleBackendRoutesByRepoStorageID(t *testing.T) {
	local := &recordingBackend{}
	remote := &recordingBackend{}
	multi := &multiBackend{
		backends: map[string]storageBackend{
			"local":  local,
			"remote": remote,
		},
		defaultID: "local",
		storageIDForRepo: func(_ context.Context, repoID string) (string, error) {
			if repoID == "remote-repo" {
				return "remote", nil
			}
			return "", sql.ErrNoRows
		},
	}

	if err := multi.write("remote-repo", objID, bytes.NewReader(nil), false); err != nil {
		t.Fatal(err)
	}
	if err := multi.write("unmapped-repo", objID, bytes.NewReader(nil), false); err != nil {
		t.Fatal(err)
	}
	if remote.writes != 1 || local.writes != 1 {
		t.Fatalf("writes remote=%d local=%d, want 1 each", remote.writes, local.writes)
	}
}

func TestS3BackendIntegration(t *testing.T) {
	endpoint := os.Getenv("CF_S3_TEST_ENDPOINT")
	if endpoint == "" {
		t.Skip("set CF_S3_TEST_ENDPOINT to run against MinIO")
	}
	conf, err := os.CreateTemp(t.TempDir(), "seafile.conf")
	if err != nil {
		t.Fatal(err)
	}
	_, err = conf.WriteString("[commit_object_backend]\nname = s3\nbucket = cloudfile-commits\nkey_id = minioadmin\nkey = minioadmin\nhost = " + endpoint + "\nuse_https = false\nuse_v4_signature = true\npath_style_request = true\n")
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
	s3 := backend.(*s3Backend)
	if err := s3.client.MakeBucket(context.Background(), s3.bucket, minio.MakeBucketOptions{}); err != nil {
		response := minio.ToErrorResponse(err)
		if response.Code != "BucketAlreadyOwnedByYou" && response.Code != "BucketAlreadyExists" {
			t.Fatalf("make bucket: %s: %v", response.Code, err)
		}
	}
	if err := backend.write(repoID, objID, bytes.NewBufferString("s3 verification"), false); err != nil {
		t.Fatal(err)
	}
	var out bytes.Buffer
	if err := backend.read(repoID, objID, &out); err != nil {
		t.Fatal(err)
	}
	if out.String() != "s3 verification" {
		t.Fatalf("read %q", out.String())
	}
	if size, err := backend.stat(repoID, objID); err != nil || size != int64(out.Len()) {
		t.Fatalf("stat = %d, %v", size, err)
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
