package objstore

import (
	"bytes"
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestAutoLocalSharedCases(t *testing.T) {
	source := os.Getenv("CF_AUTO_STORAGE_CASES")
	if source == "" {
		source = "../../../cloudfile-docker/docs/features/automatic-local-storage-cases.json"
	}
	body, err := os.ReadFile(source)
	if err != nil {
		t.Fatal(err)
	}
	var cases []struct {
		Key   string `json:"key"`
		Valid bool   `json:"valid"`
	}
	if err = json.Unmarshal(body, &cases); err != nil {
		t.Fatal(err)
	}
	for _, c := range cases {
		_, valid := autoLocalKey("auto-local:" + c.Key)
		if valid != c.Valid {
			t.Fatalf("key %q: got %v", c.Key, valid)
		}
	}
}

func TestAutoLocalIsolationAndColdLookup(t *testing.T) {
	parent := t.TempDir()
	router := func(_ context.Context, repo string) (string, error) { return "auto-local:" + repo, nil }
	makeBackend := func() *multiBackend {
		return &multiBackend{backends: map[string]storageBackend{}, autoRoot: parent, objType: "blocks", storageIDForRepo: router}
	}
	m := makeBackend()
	for _, key := range []string{"dept-one", "dept-two"} {
		if err := m.write(key, objID, bytes.NewBufferString(key), false); err != nil {
			t.Fatal(err)
		}
	}
	// A fresh process must reconstruct the same children without a per-key
	// deployment entry, even when object IDs happen to be identical.
	for _, key := range []string{"dept-one", "dept-two"} {
		var out bytes.Buffer
		if err := makeBackend().read(key, objID, &out); err != nil {
			t.Fatal(err)
		}
		if out.String() != key {
			t.Fatalf("cross-library data: %q", out.String())
		}
	}
	if _, err := m.backend("../outside"); err == nil {
		t.Fatal("accepted traversal")
	}
}

func TestAutoLocalRejectsSymlinkAndAbsentParent(t *testing.T) {
	parent, outside := t.TempDir(), t.TempDir()
	if err := os.Symlink(outside, filepath.Join(parent, "redirect")); err != nil {
		t.Fatal(err)
	}
	if _, err := autoLocalDir(parent, "redirect"); err == nil {
		t.Fatal("followed key symlink")
	}
	if _, err := autoLocalDir(filepath.Join(parent, "missing"), "new-key"); err == nil {
		t.Fatal("created missing mount")
	}
}
