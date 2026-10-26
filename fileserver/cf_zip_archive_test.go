package main

import (
	"archive/zip"
	"bytes"
	"context"
	"crypto/sha1"
	"database/sql"
	"database/sql/driver"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/haiwen/seafile-server/fileserver/blockmgr"
	"github.com/haiwen/seafile-server/fileserver/commitmgr"
	"github.com/haiwen/seafile-server/fileserver/fsmgr"
	"github.com/haiwen/seafile-server/fileserver/option"
	"github.com/haiwen/seafile-server/fileserver/repomgr"
)

// Inspect real native metadata, blocks and ZIP bytes in a temporary store.
func TestNativeZipArchiveBytes(t *testing.T) {
	const rid = "11111111-1111-4111-8111-111111111111"
	tmp := t.TempDir()
	conf, data := filepath.Join(tmp, "missing-config"), filepath.Join(tmp, "data")
	fsmgr.Init(conf, data, 1<<20)
	blockmgr.Init(conf, data)
	file := func(name string, body []byte) *fsmgr.SeafDirent {
		t.Helper()
		ids := []string{}
		for p := 0; p < len(body); p += 7 {
			end := p + 7
			if end > len(body) {
				end = len(body)
			}
			chunk := body[p:end]
			id := fmt.Sprintf("%x", sha1.Sum(chunk))
			ids = append(ids, id)
			if err := blockmgr.Write(rid, id, bytes.NewReader(chunk)); err != nil {
				t.Fatal(err)
			}
		}
		f, err := fsmgr.NewSeafile(1, int64(len(body)), ids)
		if err != nil {
			t.Fatal(err)
		}
		if err = fsmgr.SaveSeafile(rid, f); err != nil {
			t.Fatal(err)
		}
		return &fsmgr.SeafDirent{Name: name, ID: f.FileID, Mode: 0100644, Size: int64(len(body))}
	}
	dir := func(name string, entries ...*fsmgr.SeafDirent) *fsmgr.SeafDirent {
		t.Helper()
		d, err := fsmgr.NewSeafdir(1, entries)
		if err != nil {
			t.Fatal(err)
		}
		if err = fsmgr.SaveSeafdir(rid, d); err != nil {
			t.Fatal(err)
		}
		return &fsmgr.SeafDirent{Name: name, ID: d.DirID, Mode: 0040755}
	}
	// Incompressible content forces HTTP bytes out before the next file fails.
	binary := make([]byte, 16384)
	for p := 0; p < len(binary); p += sha1.Size {
		sum := sha1.Sum([]byte(fmt.Sprint(p)))
		copy(binary[p:], sum[:])
	}
	chinese := []byte("中文内容\nsecond line\n")
	nested := []byte("different bytes with the same basename")
	a := file("a.xlsx", binary)
	b := file("方案 +记录.txt", chinese)
	empty := file("empty.txt", nil)
	folder := dir("folder", file("a.xlsx", nested), dir("empty-dir"))
	ignored := file("not-selected.txt", []byte("must not appear"))
	root := dir("root", dir("资料", a, b, empty, folder, ignored))
	repo := &repomgr.Repo{ID: rid, StoreID: rid, RootID: root.ID, Version: 1}

	// Only Repo/Branch lookup is a fixture; metadata, blocks, ZIP and HTTP are real.
	oldTimeout := option.DBOpTimeout
	option.DBOpTimeout = time.Second
	defer func() { option.DBOpTimeout = oldTimeout }()
	commitmgr.Init(conf, data)
	openRepo := func(rootID string) *sql.DB {
		t.Helper()
		c := commitmgr.NewCommit(rid, "", rootID, "tester", "ZIP regression")
		c.Version = 1
		if err := commitmgr.Save(c); err != nil {
			t.Fatal(err)
		}
		db := sql.OpenDB(zipInspectConnector{rid, c.CommitID})
		repomgr.Init(db)
		return db
	}
	db := openRepo(root.ID)
	defer db.Close()
	cases := []struct {
		name  string
		names []interface{}
		want  map[string][]byte
	}{
		{"multiple files", []interface{}{a.Name, b.Name, empty.Name}, map[string][]byte{a.Name: binary, b.Name: chinese, empty.Name: nil}},
		{"files and folder", []interface{}{a.Name, b.Name, folder.Name, empty.Name}, map[string][]byte{a.Name: binary, b.Name: chinese, empty.Name: nil, "folder/a.xlsx": nested, "folder/empty-dir/": nil}},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {

			payload, err := json.Marshal(map[string]interface{}{"parent_dir": "/资料", "file_list": tc.names})
			if err != nil {
				t.Fatal(err)
			}
			rsp := httptest.NewRecorder()
			if failure := downloadZipFile(rsp, httptest.NewRequest("GET", "/zip/test", nil), string(payload), rid, "tester", "download-multi"); failure != nil {
				t.Fatal(failure)
			}
			out := rsp.Body

			z, err := zip.NewReader(bytes.NewReader(out.Bytes()), int64(out.Len()))
			if err != nil {
				t.Fatal(err)
			}
			if len(z.File) != len(tc.want) {
				t.Fatalf("%d entries, want %d", len(z.File), len(tc.want))
			}
			seen := map[string]bool{}
			for _, e := range z.File {
				want, ok := tc.want[e.Name]
				if !ok || seen[e.Name] {
					t.Fatalf("unexpected/duplicate %s", e.Name)
				}
				seen[e.Name] = true
				r, err := e.Open()
				if err != nil {
					t.Fatal(err)
				}
				got, err := io.ReadAll(r)
				r.Close()
				if err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(got, want) {
					t.Fatalf("content mismatch %s", e.Name)
				}
				t.Logf("verified %s: %d bytes", e.Name, len(got))
			}
		})
	}
	t.Run("missing file", func(t *testing.T) {
		got, err := parseDirFilelist(repo, map[string]interface{}{"parent_dir": "/资料", "file_list": []interface{}{a.Name, "missing.txt"}})
		if err == nil || got != nil {
			t.Fatalf("missing file accepted: %v %v", got, err)
		}
	})

	t.Run("block read failure aborts HTTP", func(t *testing.T) {
		// Missing blocks after metadata succeeds must never yield a valid partial ZIP.
		f, err := fsmgr.NewSeafile(1, 99, []string{strings.Repeat("f", 40)})
		if err != nil {
			t.Fatal(err)
		}
		if err = fsmgr.SaveSeafile(rid, f); err != nil {
			t.Fatal(err)
		}
		broken := &fsmgr.SeafDirent{Name: "broken.txt", ID: f.FileID, Mode: 0100644, Size: 99}
		badFolder := dir("bad-folder", broken)
		badRoot := dir("root", a, broken, badFolder)
		badDB := openRepo(badRoot.ID)
		defer badDB.Close()
		for _, tc := range []struct{ name, op, data string }{
			{"file in multi-selection", "download-multi", `{"parent_dir":"/","file_list":["a.xlsx","broken.txt"]}`},
			{"folder in multi-selection", "download-multi", `{"parent_dir":"/","file_list":["a.xlsx","bad-folder"]}`},
			{"single folder", "download-dir", fmt.Sprintf(`{"parent_dir":"/","dir_name":"bad-folder","obj_id":%q}`, badFolder.ID)},
		} {
			t.Run(tc.name, func(t *testing.T) {
				server := httptest.NewServer(appHandler(func(w http.ResponseWriter, r *http.Request) *appError {
					return downloadZipFile(w, r, tc.data, rid, "tester", tc.op)
				}))
				defer server.Close()
				response, err := server.Client().Get(server.URL)
				if err != nil {
					if tc.op == "download-multi" {
						t.Fatalf("expected partial response before abort: %v", err)
					}
					return
				} // Single-folder failure can abort before response headers.
				defer response.Body.Close()
				body, readErr := io.ReadAll(response.Body)
				t.Logf("interrupted response: HTTP %d, bytes=%d, error=%v", response.StatusCode, len(body), readErr)
				if readErr == nil {
					t.Fatalf("failed download completed normally with HTTP %d", response.StatusCode)
				}
				if _, err = zip.NewReader(bytes.NewReader(body), int64(len(body))); err == nil {
					t.Fatal("failed transfer finalized a valid ZIP")
				}
			})
		}
	})

}

type zipInspectConnector struct{ rid, head string }

func (c zipInspectConnector) Driver() driver.Driver { return uploadGuardDriver{} }
func (c zipInspectConnector) Connect(context.Context) (driver.Conn, error) {
	return zipInspectConn{c}, nil
}

type zipInspectConn struct{ c zipInspectConnector }

func (c zipInspectConn) Prepare(string) (driver.Stmt, error) { return zipInspectStmt{c.c}, nil }
func (c zipInspectConn) Close() error                        { return nil }
func (c zipInspectConn) Begin() (driver.Tx, error)           { return uploadGuardTx{}, nil }

type zipInspectStmt struct{ c zipInspectConnector }

func (s zipInspectStmt) Close() error  { return nil }
func (s zipInspectStmt) NumInput() int { return 1 }
func (s zipInspectStmt) Exec([]driver.Value) (driver.Result, error) {
	return nil, fmt.Errorf("read only")
}
func (s zipInspectStmt) Query([]driver.Value) (driver.Rows, error) {
	return &uploadGuardRows{names: []string{"repo", "head", "origin", "path", "base"}, values: [][]driver.Value{{s.c.rid, s.c.head, nil, nil, nil}}}, nil
}
