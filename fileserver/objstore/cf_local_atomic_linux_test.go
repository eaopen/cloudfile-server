package objstore

// Requires a disposable Linux mount namespace; never use live data paths.
import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"syscall"
	"testing"
)

func TestLocalAtomicBindMount(t *testing.T) {
	if os.Getenv("CF_TEST_BIND_MOUNTS") != "1" {
		t.Skip("requires isolated mount-capable Linux container")
	}
	for _, scope := range []string{"backend", "repository", "shard"} {
		t.Run(scope, func(t *testing.T) {
			root := t.TempDir()
			b, err := newFSBackend(root, "blocks")
			if err != nil {
				t.Fatal(err)
			}
			target := filepath.Join(root, "storage", "blocks")
			if scope != "backend" {
				target = filepath.Join(target, repoID)
			}
			if scope == "shard" {
				target = filepath.Join(target, objID[:2])
			}
			if err = os.MkdirAll(target, 0700); err != nil {
				t.Fatal(err)
			}
			backing := t.TempDir()
			if err = syscall.Mount(backing, target, "", syscall.MS_BIND, ""); err != nil {
				t.Fatal(err)
			}
			defer syscall.Unmount(target, 0)
			probe := filepath.Join(root, "old-temp-probe")
			if err = os.WriteFile(probe, []byte("x"), 0600); err != nil {
				t.Fatal(err)
			}
			if err = os.Rename(probe, filepath.Join(target, "old-temp-probe")); !errors.Is(err, syscall.EXDEV) {
				t.Fatalf("fixture did not reproduce EXDEV: %v", err)
			}
			payload := []byte("same-directory staging survives bind mount")
			if err = b.write(repoID, objID, bytes.NewReader(payload), true); err != nil {
				t.Fatal(err)
			}
			var got bytes.Buffer
			if err = b.read(repoID, objID, &got); err != nil || !bytes.Equal(got.Bytes(), payload) {
				t.Fatal("mounted roundtrip failed")
			}
		})
	}
}
