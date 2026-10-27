package objstore

import (
	"container/list"
	"context"
	"database/sql"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"
	"gopkg.in/ini.v1"
)

// Set before object stores are initialized. No second pool or credentials.
var v2StorageDB *sql.DB

func InitV2Storage(db *sql.DB) { v2StorageDB = db }

// Repository storage routes are immutable during normal service life. Keep only
// verified positive mappings, with a short TTL for offline repair/GC changes.
// This is a physical-path cache, never an authorization or object-content cache.
const (
	v2RouteCacheCapacity  = 1024
	v2RouteCacheTTL       = 10 * time.Second
	v2NativeRouteCacheTTL = time.Second
)

type v2CachedRoute struct {
	repoID      string
	route       string
	legacyKey   string
	legacyState string
	expires     time.Time
}

type v2StorageBackend struct {
	native *fsBackend
	root   string
	lookup func(string) (string, string, string, error)

	cacheMu    sync.Mutex
	cacheIndex map[string]*list.Element
	cacheLRU   list.List
}

func (b *v2StorageBackend) cachedRoute(repoID string) (string, string, string, bool) {
	b.cacheMu.Lock()
	defer b.cacheMu.Unlock()
	if elem := b.cacheIndex[repoID]; elem != nil {
		entry := elem.Value.(*v2CachedRoute)
		if time.Now().Before(entry.expires) {
			b.cacheLRU.MoveToFront(elem)
			return entry.route, entry.legacyKey, entry.legacyState, true
		}
		b.cacheLRU.Remove(elem)
		delete(b.cacheIndex, repoID)
	}
	return "", "", "", false
}

func (b *v2StorageBackend) rememberRoute(repoID, route, legacyKey, legacyState string) {
	b.cacheMu.Lock()
	defer b.cacheMu.Unlock()
	if b.cacheIndex == nil {
		b.cacheIndex = make(map[string]*list.Element)
	}
	if elem := b.cacheIndex[repoID]; elem != nil {
		b.cacheLRU.Remove(elem)
	}
	ttl := v2RouteCacheTTL
	if route == "" { // CE/VirtualRepo lifecycle facts are not immutable.
		ttl = v2NativeRouteCacheTTL
	}
	entry := &v2CachedRoute{repoID, route, legacyKey, legacyState, time.Now().Add(ttl)}
	b.cacheIndex[repoID] = b.cacheLRU.PushFront(entry)
	if b.cacheLRU.Len() > v2RouteCacheCapacity {
		oldest := b.cacheLRU.Remove(b.cacheLRU.Back()).(*v2CachedRoute)
		delete(b.cacheIndex, oldest.repoID)
	}
}

// Test seam for an injected out-of-band route change. No production
// management API invokes this: live physical routes are immutable, and offline
// repair requires stopped writers and process restart. The TTL only limits
// incidental staleness; it is not an online migration protocol.
func (b *v2StorageBackend) invalidateRoute(repoID string) {
	b.cacheMu.Lock()
	defer b.cacheMu.Unlock()
	if elem := b.cacheIndex[repoID]; elem != nil {
		b.cacheLRU.Remove(elem)
		delete(b.cacheIndex, repoID)
	}
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

	// RepoStorageId alone selects the physical route. Look at the legacy
	// registry and Seafile native lifecycle only if there is NO route:
	// otherwise a missing historical auto-local mapping could silently write
	// to the wrong (native) path. New UUID V2 carries a native route plus an
	// immutable safety marker, never a second routing source.
	//
	// VirtualRepo is a native resource too: its row exists before the initial
	// commit while the Repo row is written afterwards, so folder-share
	// sub-libraries must still resolve to the native tree.
	var route, routeOwner, legacyKey, legacyState, nativeRepo sql.NullString
	err := v2StorageDB.QueryRowContext(ctx,
		"SELECT s.storage_id,s.repo_id,c.repo_key,c.state,"+
			"COALESCE(r.repo_id,t.repo_id,g.repo_id,v.repo_id) "+
			"FROM (SELECT ? AS repo_id) i "+
			"LEFT JOIN RepoStorageId s ON s.repo_id=i.repo_id "+
			"LEFT JOIN cf_library_storage c ON c.repo_id=i.repo_id AND s.repo_id IS NULL "+
			"LEFT JOIN Repo r ON r.repo_id=i.repo_id AND s.repo_id IS NULL "+
			"LEFT JOIN RepoTrash t ON t.repo_id=i.repo_id AND s.repo_id IS NULL "+
			"LEFT JOIN GarbageRepos g ON g.repo_id=i.repo_id AND s.repo_id IS NULL "+
			"LEFT JOIN VirtualRepo v ON v.repo_id=i.repo_id AND s.repo_id IS NULL", repoID).
		Scan(&route, &routeOwner, &legacyKey, &legacyState, &nativeRepo)
	if err != nil {
		return "", "", "", err
	}
	if routeOwner.Valid {
		if !route.Valid || route.String == "" {
			return "", "", "", fmt.Errorf("V2 route row has no storage ID")
		}
		return route.String, "", "", nil
	}
	nativeMarker := !legacyKey.Valid ||
		(legacyKey.String == "native-"+repoID &&
			(legacyState.String == "active" || legacyState.String == "retired"))
	if !nativeRepo.Valid || !nativeMarker {
		return "", "", "", fmt.Errorf("unresolved native Seafile library")
	}
	return "", legacyKey.String, legacyState.String, nil
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
	route, legacyKey, legacyState, cached := b.cachedRoute(repoID)
	if !cached {
		var err error
		route, legacyKey, legacyState, err = b.lookup(repoID)
		if err != nil {
			return nil, err
		}
	}
	child, err := b.resolveRoute(repoID, route, legacyKey, legacyState)
	if err != nil {
		return nil, err
	}
	if !cached {
		b.rememberRoute(repoID, route, legacyKey, legacyState)
	}
	return child, nil
}

func (b *v2StorageBackend) resolveRoute(repoID, route, legacyKey, legacyState string) (*fsBackend, error) {
	if route == "" {
		// Native CE and virtual libraries remain on their original filesystem.
		// A lost managed route must never turn into a native fallback.
		if legacyKey == "" || (legacyKey == "native-"+repoID &&
			(legacyState == "active" || legacyState == "retired")) {
			return b.native, nil
		}
		return nil, fmt.Errorf("unresolved historical native binding")
	}
	if !strings.HasPrefix(route, "auto-local:") {
		return nil, fmt.Errorf("unsupported V2 storage route")
	}
	key := strings.TrimPrefix(route, "auto-local:")
	if !v2KeyValid(key) {
		return nil, fmt.Errorf("invalid V2 storage directory key")
	}
	// Validate the mount on every object operation, including cache hits.
	// A missing or replaced mount must not redirect writes into a new tree.
	if err := v2RealDirectory(b.root); err != nil {
		return nil, err
	}
	keyDir := filepath.Join(b.root, key)
	if err := v2RealDirectory(keyDir); err != nil {
		return nil, err
	}
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
