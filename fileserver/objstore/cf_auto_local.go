package objstore

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// A storage key is a directory component, never a client-selected path.
// Match common/cf-auto-storage.h and the shared cross-layer fixtures.
func autoLocalKey(storageID string) (string, bool) {
	const prefix = "auto-local:"
	if !strings.HasPrefix(storageID, prefix) {
		return "", false
	}
	key := strings.TrimPrefix(storageID, prefix)
	if len(key) == 0 || len(key) > 80 {
		return "", false
	}
	for i := 0; i < len(key); i++ {
		c := key[i]
		alnum := c >= 'a' && c <= 'z' || c >= '0' && c <= '9'
		if !alnum && !(c == '-' && i > 0 && i+1 < len(key)) {
			return "", false
		}
	}
	return key, true
}

func autoLocalDir(parent, key string) (string, error) {
	if _, valid := autoLocalKey("auto-local:" + key); !valid || !filepath.IsAbs(parent) {
		return "", fmt.Errorf("invalid automatic local directory")
	}
	// The parent must already be mounted. Do not recreate an absent mount or
	// follow a symlink at the managed key boundary during lazy allocation.
	info, err := os.Lstat(parent)
	if err != nil {
		return "", err
	}
	if !info.IsDir() {
		return "", fmt.Errorf("automatic local parent is not a directory")
	}
	dir := filepath.Join(parent, key)
	if err = os.Mkdir(dir, 0700); err != nil && !os.IsExist(err) {
		return "", err
	}
	info, err = os.Lstat(dir)
	if err != nil {
		return "", err
	}
	if !info.IsDir() {
		return "", fmt.Errorf("automatic local key is not a directory")
	}
	return dir, nil
}
