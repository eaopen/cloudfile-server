// Package objstore provides operations for commit, fs and block objects.
// It is low-level package used by commitmgr, fsmgr, blockmgr packages to access storage.
package objstore

import (
	"fmt"
	"io"
	"os"

	"gopkg.in/ini.v1"
)

// ObjectStore is a container to access storage backend
type ObjectStore struct {
	// can be "commit", "fs", or "block"
	ObjType string
	backend storageBackend
}

// storageBackend is the interface implemented by storage backends.
// An object store may have one or multiple storage backends.
type storageBackend interface {
	// Read an object from backend and write the contents into w.
	read(repoID string, objID string, w io.Writer) (err error)
	// Write the contents from r to the object.
	write(repoID string, objID string, r io.Reader, sync bool) (err error)
	// exists checks whether an object exists.
	exists(repoID string, objID string) (res bool, err error)
	// stat calculates an object's size
	stat(repoID string, objID string) (res int64, err error)
}

// New returns a new object store for a given type of objects.
// objType can be "commit", "fs", or "block".
func New(seafileConfPath string, seafileDataDir string, objType string) *ObjectStore {
	obj := new(ObjectStore)
	obj.ObjType = objType
	backend, err := newBackend(seafileConfPath, seafileDataDir, objType)
	if err != nil {
		panic(fmt.Sprintf("initialize %s object store: %v", objType, err))
	}
	obj.backend = backend
	return obj
}

func newBackend(seafileConfPath, seafileDataDir, objType string) (storageBackend, error) {
	config, err := ini.Load(seafileConfPath)
	if err != nil {
		if info, statErr := os.Stat(seafileConfPath); os.IsNotExist(err) || (statErr == nil && info.IsDir()) {
			return newFSBackend(seafileDataDir, objType)
		}
		return nil, err
	}

	sectionName := map[string]string{
		"commits": "commit_object_backend",
		"fs":      "fs_object_backend",
		"blocks":  "block_backend",
	}[objType]
	if sectionName == "" {
		return nil, fmt.Errorf("unknown object type %q", objType)
	}
	section, err := config.GetSection(sectionName)
	if err != nil || section.Key("name").String() == "" || section.Key("name").String() == "filesystem" {
		return newFSBackend(seafileDataDir, objType)
	}
	if section.Key("name").String() != "s3" {
		return nil, fmt.Errorf("unsupported %s backend %q", objType, section.Key("name").String())
	}
	return newS3Backend(section)
}

// Read data from storage backends.
func (s *ObjectStore) Read(repoID string, objID string, w io.Writer) (err error) {
	return s.backend.read(repoID, objID, w)
}

// Write data to storage backends.
func (s *ObjectStore) Write(repoID string, objID string, r io.Reader, sync bool) (err error) {
	return s.backend.write(repoID, objID, r, sync)
}

// Check whether object exists.
func (s *ObjectStore) Exists(repoID string, objID string) (res bool, err error) {
	return s.backend.exists(repoID, objID)
}

// Stat calculates object size.
func (s *ObjectStore) Stat(repoID string, objID string) (res int64, err error) {
	return s.backend.stat(repoID, objID)
}
