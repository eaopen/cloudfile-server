package objstore

import (
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"

	_ "github.com/go-sql-driver/mysql"
	"github.com/haiwen/seafile-server/fileserver/option"
	"gopkg.in/ini.v1"
)

type storageClass struct {
	StorageID string                 `json:"storage_id"`
	Default   bool                   `json:"is_default"`
	Commits   map[string]interface{} `json:"commits"`
	FS        map[string]interface{} `json:"fs"`
	Blocks    map[string]interface{} `json:"blocks"`
}

type multiBackend struct {
	backends  map[string]storageBackend
	defaultID string
	db        *sql.DB
}

func newMultiBackend(config *ini.File, seafileConfPath, objType string) (*multiBackend, error) {
	storage, err := config.GetSection("storage")
	if err != nil || !storage.Key("enable_storage_classes").MustBool(false) {
		return nil, fmt.Errorf("multiple backend requires [storage] enable_storage_classes = true")
	}
	classesPath := storage.Key("storage_classes_file").String()
	if classesPath == "" {
		return nil, fmt.Errorf("multiple backend requires storage_classes_file")
	}
	body, err := os.ReadFile(classesPath)
	if err != nil {
		return nil, err
	}
	var classes []storageClass
	if err := json.Unmarshal(body, &classes); err != nil {
		return nil, err
	}

	m := &multiBackend{backends: make(map[string]storageBackend)}
	for _, class := range classes {
		if class.StorageID == "" {
			return nil, fmt.Errorf("storage class has no storage_id")
		}
		if class.Default {
			if m.defaultID != "" {
				return nil, fmt.Errorf("multiple default storage classes")
			}
			m.defaultID = class.StorageID
		}
		var spec map[string]interface{}
		switch objType {
		case "commits":
			spec = class.Commits
		case "fs":
			spec = class.FS
		case "blocks":
			spec = class.Blocks
		}
		backend, err := backendFromClassSpec(spec, objType)
		if err != nil {
			return nil, fmt.Errorf("storage class %s: %w", class.StorageID, err)
		}
		m.backends[class.StorageID] = backend
	}
	if m.defaultID == "" {
		return nil, fmt.Errorf("storage classes require one is_default")
	}

	dbOption, err := option.LoadDBOption(filepath.Dir(seafileConfPath))
	if err != nil {
		return nil, err
	}
	dsn := fmt.Sprintf("%s:%s@tcp(%s:%d)/%s?parseTime=true", dbOption.User, dbOption.Password, dbOption.Host, dbOption.Port, dbOption.SeafileDbName)
	m.db, err = sql.Open("mysql", dsn)
	if err != nil {
		return nil, err
	}
	return m, nil
}

func backendFromClassSpec(spec map[string]interface{}, objType string) (storageBackend, error) {
	if spec == nil {
		return nil, fmt.Errorf("missing %s backend", objType)
	}
	backend, _ := spec["backend"].(string)
	switch backend {
	case "fs":
		dir, _ := spec["dir"].(string)
		if dir == "" {
			return nil, fmt.Errorf("fs backend requires dir")
		}
		return newFSBackend(dir, objType)
	case "s3":
		file := ini.Empty()
		section, _ := file.NewSection("s3")
		for key, value := range spec {
			section.NewKey(key, fmt.Sprint(value))
		}
		return newS3Backend(section)
	default:
		return nil, fmt.Errorf("unsupported backend %q", backend)
	}
}

func (m *multiBackend) backend(repoID string) (storageBackend, error) {
	storageID := m.defaultID
	err := m.db.QueryRowContext(context.Background(), "SELECT storage_id FROM RepoStorageId WHERE repo_id = ? LIMIT 1", repoID).Scan(&storageID)
	if err != nil && err != sql.ErrNoRows {
		return nil, err
	}
	backend, ok := m.backends[storageID]
	if !ok {
		return nil, fmt.Errorf("repo %s references unknown storage class %q", repoID, storageID)
	}
	return backend, nil
}

func (m *multiBackend) read(repoID, objID string, w io.Writer) error {
	b, e := m.backend(repoID)
	if e != nil {
		return e
	}
	return b.read(repoID, objID, w)
}
func (m *multiBackend) write(repoID, objID string, r io.Reader, sync bool) error {
	b, e := m.backend(repoID)
	if e != nil {
		return e
	}
	return b.write(repoID, objID, r, sync)
}
func (m *multiBackend) exists(repoID, objID string) (bool, error) {
	b, e := m.backend(repoID)
	if e != nil {
		return false, e
	}
	return b.exists(repoID, objID)
}
func (m *multiBackend) stat(repoID, objID string) (int64, error) {
	b, e := m.backend(repoID)
	if e != nil {
		return -1, e
	}
	return b.stat(repoID, objID)
}
