package main

import (
	"context"
	"database/sql"
	"errors"
	"io"
	"net/http"
	"time"

	"github.com/haiwen/seafile-server/fileserver/option"
)

var errCloudFileLegacyLibrary = errors.New("legacy library access unavailable")

// Missing/drifted schema is not an unmanaged library. Lock marker gaps through
// final publication, so enrollment cannot race a successful legacy commit.
func checkLegacyLibrary(ctx context.Context, tx *sql.Tx, repoID string) error {
	if !option.CloudFileManagedLibraryGuard {
		var installed int
		err := tx.QueryRowContext(ctx, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name IN ('cf_schema_migration','cf_managed_library')").Scan(&installed)
		if err != nil {
			return errCloudFileLegacyLibrary
		}
		if installed == 0 {
			return nil
		}
	}
	if len(repoID) != 36 {
		return errCloudFileLegacyLibrary
	}
	rows, err := tx.QueryContext(ctx, "SELECT repo_id FROM cf_managed_library LIMIT 0 FOR UPDATE")
	if err != nil {
		return errCloudFileLegacyLibrary
	}
	err = rows.Close()
	if err != nil {
		return errCloudFileLegacyLibrary
	}
	checks := []string{
		"SELECT COUNT(*) FROM cf_schema_migration WHERE version='032_managed_libraries' AND state='applied'",
		"SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name='cf_managed_library' AND ENGINE='InnoDB'",
		"SELECT COUNT(*) FROM (SELECT table_name FROM information_schema.columns WHERE table_schema=DATABASE() AND table_name='cf_managed_library' GROUP BY table_name HAVING COUNT(*)=2 AND SUM(column_name='repo_id' AND data_type='char' AND character_maximum_length=36 AND collation_name='ascii_bin' AND is_nullable='NO')=1 AND SUM(column_name='created_at' AND data_type='datetime' AND datetime_precision=6 AND is_nullable='NO')=1) AS shape",
		"SELECT COUNT(*) FROM (SELECT index_name FROM information_schema.statistics WHERE table_schema=DATABASE() AND table_name='cf_managed_library' AND index_name='PRIMARY' GROUP BY index_name HAVING COUNT(*)=1 AND SUM(column_name='repo_id' AND seq_in_index=1 AND sub_part IS NULL AND non_unique=0)=1) AS shape",
	}
	for _, query := range checks {
		var count int
		if tx.QueryRowContext(ctx, query).Scan(&count) != nil || count != 1 {
			return errCloudFileLegacyLibrary
		}
	}
	var isolation string
	err = tx.QueryRowContext(ctx, "SELECT @@transaction_isolation").Scan(&isolation)
	if err != nil {
		err = tx.QueryRowContext(ctx, "SELECT @@tx_isolation").Scan(&isolation)
	}
	if err != nil || (isolation != "REPEATABLE-READ" && isolation != "SERIALIZABLE") {
		return errCloudFileLegacyLibrary
	}
	var origin string
	err = tx.QueryRowContext(ctx, "SELECT origin_repo FROM VirtualRepo WHERE repo_id=? FOR UPDATE", repoID).Scan(&origin)
	if err != nil && err != sql.ErrNoRows {
		return errCloudFileLegacyLibrary
	}
	if err == nil && len(origin) != 36 {
		return errCloudFileLegacyLibrary
	}
	first, second := repoID, origin
	if second != "" && first > second {
		first, second = second, first
	}
	for _, id := range []string{first, second} {
		if id == "" {
			continue
		}
		var marker string
		err = tx.QueryRowContext(ctx, "SELECT repo_id FROM cf_managed_library WHERE repo_id=? FOR UPDATE", id).Scan(&marker)
		if err != sql.ErrNoRows {
			return errCloudFileLegacyLibrary
		}
	}
	return nil
}

func legacyLibraryAllowed(ctx context.Context, repoID string) error {
	if seafileDB == nil {
		return errCloudFileLegacyLibrary
	}
	deadline, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	tx, err := seafileDB.BeginTx(deadline, &sql.TxOptions{Isolation: sql.LevelRepeatableRead})
	if err != nil {
		return errCloudFileLegacyLibrary
	}
	defer tx.Rollback()
	return checkLegacyLibrary(deadline, tx, repoID)
}

// Existing enhanced transfers use their separately verified native guard.
// All ordinary file bytes/Range pass this current marker check, including
// share links and an access response obtained before library enrollment.
type cloudFileLegacyResponse struct {
	http.ResponseWriter
	request *http.Request
	repoID  string
}

func (w *cloudFileLegacyResponse) WriteHeader(status int) {
	if legacyLibraryAllowed(w.request.Context(), w.repoID) != nil {
		panic(http.ErrAbortHandler)
	}
	w.ResponseWriter.WriteHeader(status)
}

func (w *cloudFileLegacyResponse) Write(data []byte) (int, error) {
	total := 0
	for len(data) != 0 {
		if legacyLibraryAllowed(w.request.Context(), w.repoID) != nil {
			panic(http.ErrAbortHandler)
		}
		size := len(data)
		if size > cloudFileReadBlock {
			size = cloudFileReadBlock
		}
		count, err := w.ResponseWriter.Write(data[:size])
		total += count
		if err != nil {
			return total, err
		}
		if count != size {
			return total, io.ErrShortWrite
		}
		data = data[size:]
	}
	return total, nil
}
