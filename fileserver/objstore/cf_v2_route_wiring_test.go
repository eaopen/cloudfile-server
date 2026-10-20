package objstore

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Exercise the *production* object-store constructor, not a hand-made wrapper.
func TestV2ObjectStoreSelectsManagedRoute(t *testing.T) {
	root, confDir := t.TempDir(), t.TempDir()
	config := filepath.Join(confDir, "seafile.conf")
	if err := os.WriteFile(config, []byte("[cloudfile_v2]\nauto_local_root="+root+"\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if err := os.Mkdir(filepath.Join(root, repoID), 0700); err != nil {
		t.Fatal(err)
	}
	for _, kind := range []string{"commits", "fs", "blocks"} {
		t.Run(kind, func(t *testing.T) {
			for _, input := range []string{confDir, config} {
				nativeDir := t.TempDir()
				backend, err := newBackend(input, nativeDir, kind)
				if err != nil {
					t.Fatal(err)
				}
				routed, ok := backend.(*v2StorageBackend)
				if !ok {
					t.Fatalf("missing V2 route wrapper for %s", input)
				}
				routed.lookup = func(string) (string, string, string, error) {
					return "auto-local:" + repoID, repoID, "active", nil
				}
				payload := []byte("C/Go storage route contract")
				if err := backend.write(repoID, objID, bytes.NewReader(payload), true); err != nil {
					t.Fatal(err)
				}
				var actual bytes.Buffer
				if err := backend.read(repoID, objID, &actual); err != nil || !bytes.Equal(actual.Bytes(), payload) {
					t.Fatalf("managed route: %v", err)
				}
				if _, err := os.Stat(filepath.Join(nativeDir, "storage", kind, repoID)); !os.IsNotExist(err) {
					t.Fatal("V2 object leaked to native path")
				}
				// A broken registry must never be treated as an old native library.
				routed.lookup = func(string) (string, string, string, error) {
					return "auto-local:invalid", repoID, "active", nil
				}
				if err := backend.write(repoID, objID, strings.NewReader("incorrect"), false); err == nil {
					t.Fatal("invalid binding was not rejected")
				}
			}
		})
	}
}

func TestV2RootRejectsMixedLegacyStorageBackends(t *testing.T) {
	root := t.TempDir()
	configurations := []string{"[storage]\nenable_storage_classes=true\n"}
	for _, section := range []string{"commit_object_backend", "fs_object_backend", "block_backend"} {
		for _, backendName := range []string{"s3", "multiple", "multi", "unknown"} {
			configurations = append(configurations, "["+section+"]\nname="+backendName+"\n")
		}
	}
	for _, configuration := range configurations {
		for _, kind := range []string{"commits", "fs", "blocks"} {
			t.Run(kind+"/"+configuration, func(t *testing.T) {
				confDir, dataDir := t.TempDir(), t.TempDir()
				value := "[cloudfile_v2]\nauto_local_root=" + root + "\n" + configuration
				if err := os.WriteFile(filepath.Join(confDir, "seafile.conf"), []byte(value), 0600); err != nil {
					t.Fatal(err)
				}
				if _, err := newBackend(confDir, dataDir, kind); err == nil {
					t.Fatal("mixed backend must fail before creating any native backend")
				}
				entries, err := os.ReadDir(dataDir)
				if err != nil || len(entries) != 0 {
					t.Fatalf("invalid configuration performed filesystem I/O: %v, %v", entries, err)
				}
			})
		}
	}
}
