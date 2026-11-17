package main

import (
	"context"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/haiwen/seafile-server/fileserver/option"
)

// Hub calls each direct writer endpoint with a dedicated short-lived ticket.
// A successful response covers this Go process and its paired native C server.
func cfWriterCapabilities(w http.ResponseWriter, r *http.Request) {
	header := strings.Fields(r.Header.Get("Authorization"))
	if len(header) != 2 || header[0] != "Bearer" || len(header[1]) > 2048 || option.JWTPrivateKey == "" {
		cfEditFail(w, http.StatusForbidden, "FORBIDDEN")
		return
	}
	_, err := jwt.Parse(header[1], func(*jwt.Token) (interface{}, error) {
		return []byte(option.JWTPrivateKey), nil
	}, jwt.WithValidMethods([]string{"HS256"}), jwt.WithExpirationRequired(),
		jwt.WithIssuer("cloudfile-hub"), jwt.WithAudience("cf-writer-capabilities-v1"))
	if err != nil {
		cfEditFail(w, http.StatusForbidden, "FORBIDDEN")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 2*time.Second)
	defer cancel()
	reply, err := rpcclient.CallWithTimeout(time.Second, "cf_v2_writer_capabilities_v1")
	mask, valid := reply.(float64)
	if err != nil || !valid || mask != float64(int(mask)) || mask < 0 || mask > 3 {
		cfEditFail(w, http.StatusServiceUnavailable, "NATIVE_PROBE_UNAVAILABLE")
		return
	}
	checkout := int(mask)&1 != 0 && option.V2GuardAvailable && cfV2GuardRequireCurrent(ctx, seafileDB) == nil
	protection := int(mask)&2 != 0 && cfProtectionSchemaReady(ctx) == nil
	cfEditJSON(w, http.StatusOK, map[string]bool{"checkout": checkout, "protection": protection})
}

func cfProtectionSchemaReady(ctx context.Context) error {
	// The physical path must be unique and the write barrier forces the
	// descendant index. Reuse the Guard metadata representation for checks.
	rows, err := seafileDB.QueryContext(ctx,
		"SELECT COLUMN_NAME,DATA_TYPE,COALESCE(CHARACTER_MAXIMUM_LENGTH,0),IS_NULLABLE "+
			"FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='cf_file_protection'")
	if err != nil {
		return err
	}
	defer rows.Close()
	want := map[string]cfGuardColumn{
		"repo_id": {"char", 36, "NO", false}, "path_hash": {"binary", 32, "NO", false},
		"path": {"varchar", 4096, "NO", false}, "file_id": {"char", 40, "NO", false},
		"protected_by": {"varchar", 255, "NO", false}, "protected_at": {"datetime", 0, "NO", false},
	}
	for rows.Next() {
		var name string
		var got cfGuardColumn
		if err := rows.Scan(&name, &got.kind, &got.length, &got.nullable); err != nil {
			return err
		}
		if want[name] != got {
			return fmt.Errorf("unexpected protection column %s", name)
		}
		delete(want, name)
	}
	if err := rows.Err(); err != nil {
		return err
	}
	if len(want) != 0 {
		return fmt.Errorf("missing protection columns")
	}
	var primary, prefix string
	err = seafileDB.QueryRowContext(ctx, `SELECT
		COALESCE(GROUP_CONCAT(IF(INDEX_NAME='PRIMARY',part,NULL) ORDER BY seq),''),
		COALESCE(GROUP_CONCAT(IF(INDEX_NAME='cf_file_protection_prefix',part,NULL) ORDER BY seq),'')
		FROM (SELECT INDEX_NAME,SEQ_IN_INDEX seq,CONCAT(COLUMN_NAME,':',COALESCE(SUB_PART,0)) part
		FROM information_schema.STATISTICS WHERE TABLE_SCHEMA=DATABASE()
		AND TABLE_NAME='cf_file_protection') AS parts`).Scan(&primary, &prefix)
	if err == nil && (primary != "repo_id:0,path_hash:0" || prefix != "repo_id:0,path:191") {
		return fmt.Errorf("invalid protection index")
	}
	return err
}
