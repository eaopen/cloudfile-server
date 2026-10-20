// Implementation of file system storage backend.
package objstore

import (
	"errors"
	"io"
	"os"
	"path"
	"syscall"
)

type fsBackend struct {
	// Path of the object directory
	objDir  string
	objType string
}

func newFSBackend(seafileDataDir string, objType string) (*fsBackend, error) {
	objDir := path.Join(seafileDataDir, "storage", objType)
	err := os.MkdirAll(objDir, os.ModePerm)
	if err != nil {
		return nil, err
	}
	backend := new(fsBackend)
	backend.objDir = objDir
	backend.objType = objType
	return backend, nil
}

func (b *fsBackend) read(repoID string, objID string, w io.Writer) error {
	p := path.Join(b.objDir, repoID, objID[:2], objID[2:])
	fd, err := os.Open(p)
	if err != nil {
		return err
	}
	defer fd.Close()

	_, err = io.Copy(w, fd)
	if err != nil {
		return err
	}

	return nil
}

func (b *fsBackend) write(repoID string, objID string, r io.Reader, needSync bool) error {
	parentDir := path.Join(b.objDir, repoID, objID[:2])
	p := path.Join(parentDir, objID[2:])
	err := os.MkdirAll(parentDir, os.ModePerm)
	if err != nil {
		return err
	}

	// Stage beside the final object: even bind mounts of the same device reject
	// cross-mount rename. The reserved name is never a published object ID.
	tFile, err := os.CreateTemp(parentDir, ".cf-tmp-"+objID+"-*")
	if err != nil {
		return err
	}
	success := false
	defer func() {
		tFile.Close()
		if !success {
			os.Remove(tFile.Name())
		}
	}()

	_, err = io.Copy(tFile, r)
	if err != nil {
		tFile.Close()
		return err
	}

	// Honor the existing durability request before publication, as the native
	// metadata writer does; callers that do not request it retain their policy.
	if needSync {
		if err = tFile.Sync(); err != nil && !errors.Is(err, syscall.EINVAL) {
			return err
		}
	}
	err = tFile.Close()
	if err != nil {
		return err
	}

	err = os.Rename(tFile.Name(), p)
	if err != nil {
		return err
	}

	success = true
	if needSync {
		// A successful rename alone does not durably persist its directory entry.
		dir, err := os.Open(parentDir)
		if err != nil {
			return err
		}
		err = dir.Sync()
		closeErr := dir.Close()
		if err != nil && !errors.Is(err, syscall.EINVAL) {
			return err
		}
		return closeErr
	}
	return nil
}

func (b *fsBackend) exists(repoID string, objID string) (bool, error) {
	path := path.Join(b.objDir, repoID, objID[:2], objID[2:])
	_, err := os.Stat(path)
	if err != nil {
		if os.IsNotExist(err) {
			return false, err
		}
		return true, err
	}
	return true, nil
}

func (b *fsBackend) stat(repoID string, objID string) (int64, error) {
	path := path.Join(b.objDir, repoID, objID[:2], objID[2:])
	fileInfo, err := os.Stat(path)
	if err != nil {
		return -1, err
	}
	return fileInfo.Size(), nil
}
