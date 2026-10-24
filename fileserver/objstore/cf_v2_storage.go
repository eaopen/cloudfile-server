package objstore

import (
	"context"
	"database/sql"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/google/uuid"
	"gopkg.in/ini.v1"
)

// Set before object stores are initialized. No second pool or credentials.
var v2StorageDB *sql.DB

func InitV2Storage(db *sql.DB) { v2StorageDB = db }

type v2StorageBackend struct {
	native *fsBackend
	root   string
	lookup func(string) (string, string, string, error)
}

func v2KeyValid(key string) bool {
	if len(key) == 0 || len(key) > 80 {
		return false
	}
	for i, c := range key {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '-' && i > 0 && i+1 < len(key)) {
			return false
		}
	}
	return true
}

func v2RealDirectory(dir string) error {
	info, err := os.Lstat(dir)
	if err != nil {
		return err
	}
	if !info.IsDir() {
		return fmt.Errorf("V2 storage directory is not a real directory")
	}
	return nil
}

func v2Lookup(repoID string) (string, string, string, error) {
	if v2StorageDB == nil {
		return "", "", "", fmt.Errorf("V2 storage database is unavailable")
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var route, key, state, routeOwner, nativeRepo sql.NullString
	err := v2StorageDB.QueryRowContext(ctx,
		"SELECT s.storage_id,c.repo_key,c.state,s.repo_id,"+
			"COALESCE(r.repo_id,t.repo_id,g.repo_id) "+
			"FROM (SELECT ? AS repo_id) i "+
			"LEFT JOIN RepoStorageId s ON s.repo_id=i.repo_id "+
			"LEFT JOIN cf_library_storage c ON c.repo_id=i.repo_id "+
			"LEFT JOIN Repo r ON r.repo_id=i.repo_id "+
			"LEFT JOIN RepoTrash t ON t.repo_id=i.repo_id "+
			"LEFT JOIN GarbageRepos g ON g.repo_id=i.repo_id", repoID).
		Scan(&route, &key, &state, &routeOwner, &nativeRepo)
	if err == nil {
		err = validateV2Lookup(repoID, route, key, state, routeOwner, nativeRepo)
	}
	return route.String, key.String, state.String, err
}

// The legacy native fallback is valid for existing Seafile libraries only.
// Preserve all native Repo/Trash/Garbage storage locations without treating
// an arbitrary unmapped UUID or corrupt SQL row as a new filesystem library.
func validateV2Lookup(repoID string, route, key, state, routeOwner, nativeRepo sql.NullString) error {
	registeredNative := !route.Valid && key.Valid && state.Valid &&
		key.String == "native-"+repoID && (state.String == "active" || state.String == "retired")
	if (!route.Valid && key.Valid && !registeredNative) ||
		(route.Valid && !key.Valid) ||
		(routeOwner.Valid && !route.Valid) ||
		(!route.Valid && !nativeRepo.Valid) {
		return fmt.Errorf("V2 storage binding is incomplete or native repo is missing")
	}
	if (route.Valid && route.String == "") || (key.Valid && key.String == "") {
		return fmt.Errorf("V2 storage binding is empty")
	}
	return nil
}

func newV2StorageBackend(confPath string, native *fsBackend) (storageBackend, error) {
	// Fileserver callers may pass the config directory or seafile.conf itself.
	// Resolve both exactly as the existing newBackend() does.
	name := confPath
	if info, err := os.Stat(name); err == nil && info.IsDir() {
		name = filepath.Join(name, "seafile.conf")
	}
	cfg, err := ini.Load(name)
	if os.IsNotExist(err) {
		return native, nil
	}
	if err != nil {
		return nil, err
	}
	section := cfg.Section("cloudfile_v2")
	if !section.HasKey("auto_local_root") {
		return native, nil
	}
	root := section.Key("auto_local_root").String()
	if !filepath.IsAbs(root) {
		return nil, fmt.Errorf("V2 auto_local_root must be absolute")
	}
	if err := v2RealDirectory(root); err != nil {
		return nil, err
	}
	return &v2StorageBackend{native: native, root: root, lookup: v2Lookup}, nil
}

func (b *v2StorageBackend) resolve(repoID string) (*fsBackend, error) {
	if _, err := uuid.Parse(repoID); err != nil || len(repoID) != 36 {
		return nil, fmt.Errorf("invalid V2 storage repository ID")
	}
	if err := v2RealDirectory(b.root); err != nil {
		return nil, err
	}
	route, key, state, err := b.lookup(repoID)
	if err != nil {
		return nil, err
	}
	if route == "" && key == "" {
		return b.native, nil
	}
	if route == "" && key == "native-"+repoID && (state == "active" || state == "retired") {
		return b.native, nil
	}
	if !v2KeyValid(key) || !strings.HasPrefix(route, "auto-local:") || route != "auto-local:"+key ||
		(state != "reserved" && state != "active" && state != "retired") {
		return nil, fmt.Errorf("unresolved V2 storage binding")
	}
	keyDir := filepath.Join(b.root, key)
	if err := v2RealDirectory(keyDir); err != nil {
		return nil, err
	}
	// Native code retains its own fs/block identifiers, including virtual repo
	// origin IDs. Only the physical root changes. No per-request pool or cache.
	return &fsBackend{objDir: filepath.Join(keyDir, "storage", b.native.objType),
		objType: b.native.objType}, nil
}

func (b *v2StorageBackend) read(repoID, objID string, w io.Writer) error {
	child, err := b.resolve(repoID)
	if err != nil {
		return err
	}
	return child.read(repoID, objID, w)
}

func (b *v2StorageBackend) write(repoID, objID string, r io.Reader, sync bool) error {
	child, err := b.resolve(repoID)
	if err != nil {
		return err
	}
	// The object backend owns same-directory staging for both native and
	// routed libraries; never create a separate per-library tmpfiles tree.
	return child.write(repoID, objID, r, sync)
}

func (b *v2StorageBackend) exists(repoID, objID string) (bool, error) {
	child, err := b.resolve(repoID)
	if err != nil {
		return false, err
	}
	return child.exists(repoID, objID)
}

func (b *v2StorageBackend) stat(repoID, objID string) (int64, error) {
	child, err := b.resolve(repoID)
	if err != nil {
		return -1, err
	}
	return child.stat(repoID, objID)
}
