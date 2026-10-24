package objstore

import (
	"bytes"
	"errors"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func v2Fixture(t *testing.T, objType string) (*v2StorageBackend, string) {
	t.Helper()
	native, err := newFSBackend(t.TempDir(), objType)
	if err != nil {
		t.Fatal(err)
	}
	root := t.TempDir()
	if err = os.Mkdir(filepath.Join(root, repoID), 0700); err != nil {
		t.Fatal(err)
	}
	return &v2StorageBackend{native: native, root: root,
		lookup: func(string) (string, string, string, error) { return "auto-local:" + repoID, repoID, "active", nil }}, root
}

func TestV2StorageBytesAndNativeIsolation(t *testing.T) {
	for _, kind := range []string{"commits", "fs", "blocks"} {
		t.Run(kind, func(t *testing.T) {
			b, root := v2Fixture(t, kind)
			content := bytes.Repeat([]byte("V2 storage proof\x00\xff"), 8192)
			if err := b.write(repoID, objID, bytes.NewReader(content), true); err != nil {
				t.Fatal(err)
			}
			var output bytes.Buffer
			if err := b.read(repoID, objID, &output); err != nil || !bytes.Equal(content, output.Bytes()) {
				t.Fatalf("roundtrip: %v", err)
			}
			if n, err := b.stat(repoID, objID); err != nil || n != int64(len(content)) {
				t.Fatalf("stat: %d %v", n, err)
			}
			if yes, err := b.exists(repoID, objID); err != nil || !yes {
				t.Fatalf("exists: %v %v", yes, err)
			}
			physical := filepath.Join(root, repoID, "storage", kind, repoID, objID[:2], objID[2:])
			if _, err := os.Stat(physical); err != nil {
				t.Fatal(err)
			}
			if _, err := os.Stat(filepath.Join(b.native.objDir, repoID)); !os.IsNotExist(err) {
				t.Fatal("routed bytes leaked into native storage")
			}
			b.lookup = func(string) (string, string, string, error) { return "", "", "", nil }
			if err := b.write(repoID, objID, strings.NewReader("native"), true); err != nil {
				t.Fatal(err)
			}
			stored, err := os.ReadFile(physical)
			if err != nil || !bytes.Equal(stored, content) {
				t.Fatal("native write changed routed bytes")
			}
		})
	}
}

func TestV2StorageInvalidRoutesNeverFallback(t *testing.T) {
	for _, row := range [][3]string{
		{"auto-local:../escape", "../escape", "active"},
		{"auto-local:other", repoID, "active"},
		{"unknown:disk", repoID, "active"},
		{"", repoID, "active"},
	} {
		b, _ := v2Fixture(t, "blocks")
		b.lookup = func(string) (string, string, string, error) { return row[0], row[1], row[2], nil }
		if err := b.write(repoID, objID, strings.NewReader("denied"), true); err == nil {
			t.Fatalf("accepted %v", row)
		}
		if err := b.read(repoID, objID, io.Discard); err == nil {
			t.Fatalf("read accepted %v", row)
		}
		if _, err := b.exists(repoID, objID); err == nil {
			t.Fatalf("exists accepted %v", row)
		}
		if _, err := b.stat(repoID, objID); err == nil {
			t.Fatalf("stat accepted %v", row)
		}
		if _, err := os.Stat(filepath.Join(b.native.objDir, repoID)); !os.IsNotExist(err) {
			t.Fatal("fallback created native directory")
		}
	}
	b, _ := v2Fixture(t, "blocks")
	b.lookup = func(string) (string, string, string, error) { return "", "", "", errors.New("database unavailable") }
	if err := b.write(repoID, objID, strings.NewReader("denied"), true); err == nil {
		t.Fatal("database failure permitted write")
	}
}

func TestV2StorageSymlinkAndMissingMountDenied(t *testing.T) {
	b, root := v2Fixture(t, "blocks")
	if err := os.Remove(filepath.Join(root, repoID)); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink(t.TempDir(), filepath.Join(root, repoID)); err != nil {
		t.Fatal(err)
	}
	if _, err := b.resolve(repoID); err == nil {
		t.Fatal("key symlink accepted")
	}
	b.root = filepath.Join(root, "missing-mount")
	if _, err := b.resolve(repoID); err == nil {
		t.Fatal("missing mount accepted")
	}
}

func TestV2StorageConfigOffAndBadRoot(t *testing.T) {
	native, err := newFSBackend(t.TempDir(), "blocks")
	if err != nil {
		t.Fatal(err)
	}
	conf := t.TempDir()
	backend, err := newV2StorageBackend(conf, native)
	if err != nil || backend != native {
		t.Fatal("missing configuration changed native behavior")
	}
	for _, value := range []string{"", "relative", filepath.Join(conf, "missing")} {
		if err := os.WriteFile(filepath.Join(conf, "seafile.conf"), []byte("[cloudfile_v2]\nauto_local_root="+value+"\n"), 0600); err != nil {
			t.Fatal(err)
		}
		if _, err := newV2StorageBackend(conf, native); err == nil {
			t.Fatalf("bad root accepted: %q", value)
		}
	}
}

func TestV2RegisteredNativeLibraryUsesOriginalObjects(t *testing.T) {
	b, _ := v2Fixture(t, "blocks")
	b.lookup = func(string) (string, string, string, error) {
		return "", "native-" + repoID, "active", nil
	}
	payload := []byte("existing Seafile object, original UUID")
	if err := b.write(repoID, objID, bytes.NewReader(payload), true); err != nil {
		t.Fatal(err)
	}
	var got bytes.Buffer
	if err := b.read(repoID, objID, &got); err != nil || !bytes.Equal(got.Bytes(), payload) {
		t.Fatalf("native roundtrip failed: %v", err)
	}
	for _, broken := range [][3]string{
		{"", "native-other", "active"},
		{"", "native-" + repoID, "reserved"},
		{"", repoID, "active"},
	} {
		b.lookup = func(string) (string, string, string, error) {
			return broken[0], broken[1], broken[2], nil
		}
		if _, err := b.resolve(repoID); err == nil {
			t.Fatalf("accepted inconsistent historical binding: %v", broken)
		}
	}
}

func TestV2RouteIsAuthoritativeWithoutRegistryRow(t *testing.T) {
	for _, kind := range []string{"commits", "fs", "blocks"} {
		t.Run(kind, func(t *testing.T) {
			b, root := v2Fixture(t, kind)
			oldRepo := "a2222222-2222-4222-8222-222222222222"
			oldKey := "historical-user-key"
			if err := os.Mkdir(filepath.Join(root, oldKey), 0700); err != nil {
				t.Fatal(err)
			}
			// A fresh UUID-path library has only its native route, and a
			// historic V2 library's suffix is its immutable original path.
			for _, row := range []struct{ id, route, physical string }{
				{repoID, "auto-local:" + repoID, repoID},
				{oldRepo, "auto-local:" + oldKey, oldKey},
			} {
				b.lookup = func(string) (string, string, string, error) {
					return row.route, "", "", nil
				}
				data := []byte(row.route)
				if err := b.write(row.id, objID, bytes.NewReader(data), true); err != nil {
					t.Fatal(err)
				}
				got, err := os.ReadFile(filepath.Join(root, row.physical, "storage", kind,
					row.id, objID[:2], objID[2:]))
				if err != nil || !bytes.Equal(got, data) {
					t.Fatalf("physical route mismatch for %s: %v", row.id, err)
				}
			}
		})
	}
}

func TestV2RetiredNativeRemainsAddressableForColdGC(t *testing.T) {
	b, _ := v2Fixture(t, "blocks")
	b.lookup = func(string) (string, string, string, error) {
		return "", "native-" + repoID, "retired", nil
	}
	if resolved, err := b.resolve(repoID); err != nil || resolved != b.native {
		t.Fatalf("retired native object path must remain for GC: %v", err)
	}
}
