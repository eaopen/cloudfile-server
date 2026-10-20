package objstore

// Test actual publication behavior and deliberately unusable legacy staging.
import (
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

type atomicProbeReader struct {
	probe  func()
	reader io.Reader
}

func (r *atomicProbeReader) Read(p []byte) (int, error) {
	if r.probe != nil {
		f := r.probe
		r.probe = nil
		f()
	}
	return r.reader.Read(p)
}

type atomicFailReader struct{}

func (atomicFailReader) Read(p []byte) (int, error) {
	copy(p, "partial")
	return 7, errors.New("injected interrupted input")
}
func TestLocalAtomicStagingAndFailures(t *testing.T) {
	for _, kind := range []string{"blocks", "fs", "commits"} {
		t.Run(kind, func(t *testing.T) {
			root := t.TempDir()
			legacy := filepath.Join(root, "tmpfiles")
			if err := os.WriteFile(legacy, []byte("not a directory"), 0600); err != nil {
				t.Fatal(err)
			}
			b, err := newFSBackend(root, kind)
			if err != nil {
				t.Fatal(err)
			}
			shard := filepath.Join(root, "storage", kind, repoID, objID[:2])
			final := filepath.Join(shard, objID[2:])
			payload := []byte("complete immutable object bytes")
			r := &atomicProbeReader{reader: bytes.NewReader(payload), probe: func() {
				entries, e := os.ReadDir(shard)
				if e != nil {
					t.Fatal(e)
				}
				if len(entries) != 1 || !strings.HasPrefix(entries[0].Name(), ".cf-tmp-") {
					t.Fatalf("staging: %v", entries)
				}
				if _, e = os.Stat(final); !os.IsNotExist(e) {
					t.Fatal("object visible before completion")
				}
			}}
			if err = b.write(repoID, objID, r, true); err != nil {
				t.Fatal(err)
			}
			if err = b.write(repoID, objID, atomicFailReader{}, true); err == nil {
				t.Fatal("interrupted write succeeded")
			}
			got, e := os.ReadFile(final)
			if e != nil || !bytes.Equal(got, payload) {
				t.Fatal("failed write changed published bytes")
			}
			entries, e := os.ReadDir(shard)
			if e != nil || len(entries) != 1 || entries[0].Name() != objID[2:] {
				t.Fatalf("temp leak: %v %v", entries, e)
			}
			if got, e = os.ReadFile(legacy); e != nil || string(got) != "not a directory" {
				t.Fatal("legacy path changed")
			}
		})
	}
}
func TestLocalAtomicConcurrentPublication(t *testing.T) {
	b, err := newFSBackend(t.TempDir(), "blocks")
	if err != nil {
		t.Fatal(err)
	}
	payload := bytes.Repeat([]byte("same content-addressed object"), 4096)
	var group sync.WaitGroup
	for i := 0; i < 8; i++ {
		group.Add(1)
		go func() {
			defer group.Done()
			if e := b.write(repoID, objID, bytes.NewReader(payload), false); e != nil {
				t.Error(e)
			}
		}()
	}
	group.Wait()
	var got bytes.Buffer
	if err = b.read(repoID, objID, &got); err != nil || !bytes.Equal(got.Bytes(), payload) {
		t.Fatal("concurrent publication corrupted object")
	}
}
func TestLocalAtomicRoutedStorageIgnoresLegacyTemp(t *testing.T) {
	b, root := v2Fixture(t, "blocks")
	legacy := filepath.Join(root, repoID, "tmpfiles")
	if err := os.WriteFile(legacy, []byte("blocked legacy staging"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := b.write(repoID, objID, strings.NewReader("routed complete bytes"), true); err != nil {
		t.Fatal(err)
	}
	if data, err := os.ReadFile(legacy); err != nil || string(data) != "blocked legacy staging" {
		t.Fatal("routed staging touched legacy tmpfiles")
	}
}
