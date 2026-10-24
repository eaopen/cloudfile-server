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

// A trusted explicit repo_key is not a UUID; the physical object identifiers
// still use the actual repo UUID under that managed key's storage directory.
func TestV2StorageStableNonUUIDKey(t *testing.T) {
    b, root := v2Fixture(t, "blocks")
    key := "project-alpha"
    if err := os.Mkdir(filepath.Join(root, key), 0700); err != nil {
        t.Fatal(err)
    }
    b.lookup = func(string) (string, string, string, error) {
        return "auto-local:" + key, key, "active", nil
    }
    if err := b.write(repoID, objID, strings.NewReader("routed"), true); err != nil {
        t.Fatal(err)
    }
    physical := filepath.Join(root, key, "storage", "blocks", repoID, objID[:2], objID[2:])
    if data, err := os.ReadFile(physical); err != nil || string(data) != "routed" {
        t.Fatalf("explicit key did not route bytes: %v", err)
    }
    if _, err := os.Stat(filepath.Join(root, repoID, "storage", "blocks", repoID)); !os.IsNotExist(err) {
        t.Fatal("explicit key wrote into UUID default root")
    }
    b.lookup = func(string) (string, string, string, error) {
        return "auto-local:other", key, "active", nil
    }
    if err := b.write(repoID, objID, strings.NewReader("wrong"), false); err == nil {
        t.Fatal("mismatched key and route was accepted")
    }
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
		{"auto-local:" + repoID, repoID, "unknown"},
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
		{"auto-local:" + repoID, "native-" + repoID, "active"},
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

func TestV2RetiredNativeRemainsAddressableForColdGC(t *testing.T) {
	b, _ := v2Fixture(t, "blocks")
	b.lookup = func(string) (string, string, string, error) {
		return "", "native-" + repoID, "retired", nil
	}
	if resolved, err := b.resolve(repoID); err != nil || resolved != b.native {
		t.Fatalf("retired native object path must remain for GC: %v", err)
	}
}
