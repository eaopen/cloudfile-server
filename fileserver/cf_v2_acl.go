package main

// CF-ACL is independent of CF-EDIT. Hub owns policy semantics; native code
// supplies authenticated actors and actual paths, then rechecks at publication.
import (
	"bytes"
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"path"
	"strings"
	"time"
	"unicode/utf8"

	"github.com/haiwen/seafile-server/fileserver/commitmgr"
	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/option"
	"github.com/haiwen/seafile-server/fileserver/utils"
)

var errCFACL = errors.New("CF-ACL: denied or current authority unavailable")

type cfACLQuery interface {
	QueryRowContext(context.Context, string, ...interface{}) *sql.Row
}

// Virtual repositories cannot hide origin rules. No cache: SQL errors deny.
func cfACLProtected(ctx context.Context, db cfACLQuery, repoID string) (bool, error) {
	var protected bool
	err := db.QueryRowContext(ctx, `SELECT EXISTS(SELECT 1 FROM cf_dir_acl WHERE repo_id=? OR repo_id IN (SELECT origin_repo FROM VirtualRepo WHERE repo_id=?))`, repoID, repoID).Scan(&protected)
	return protected, err
}

// Request-path lookups are bounded like other native DB operations.
func cfACLProtectedNow(repoID string) (bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), option.DBOpTimeout)
	defer cancel()
	return cfACLProtected(ctx, seafileDB, repoID)
}

// Only endpoints without an authenticated path scope may require proof of
// whole-repository capability. This must not gate directory-aware operations.
func cfACLRequireRepoWideProof(repoID string) *appError {
	protected, err := cfACLProtectedNow(repoID)
	if err != nil || protected {
		return &appError{errCFACL, "Current path authorization required", http.StatusForbidden}
	}
	return nil
}

type cfACLCheck struct {
	Path   string `json:"path"`
	Kind   string `json:"kind"`
	Action string `json:"action"`
}

func cfACLCanonical(value string) bool {
	if value == "" || len(value) > 4096 || !utf8.ValidString(value) || value[0] != '/' || path.Clean(value) != value || strings.Contains(value, "\\") {
		return false
	}
	for _, r := range value {
		if r < 32 || r == 127 {
			return false
		}
	}
	return true
}

var cfACLClient = &http.Client{Timeout: 5 * time.Second, CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse }}

func cfACLAuthorize(ctx context.Context, repoID, user string, checks []cfACLCheck) error {
	if user == "" || len(checks) == 0 || len(checks) > 4096 {
		return errCFACL
	}
	for _, check := range checks {
		if !cfACLCanonical(check.Path) || (check.Kind != "file" && check.Kind != "dir") || (check.Action != "read" && check.Action != "write") {
			return errCFACL
		}
	}
	body, err := json.Marshal(struct {
		Username string       `json:"username"`
		RepoID   string       `json:"repo_id"`
		Checks   []cfACLCheck `json:"checks"`
	}{user, repoID, checks})
	if err != nil {
		return errCFACL
	}
	token, err := utils.GenSeahubJWTToken()
	if err != nil {
		return errCFACL
	}
	req, err := http.NewRequestWithContext(ctx, "POST", strings.TrimRight(option.SeahubURL, "/")+"/cloudfile/acl/check/", bytes.NewReader(body))
	if err != nil {
		return errCFACL
	}
	req.Header.Set("Authorization", "Token "+token)
	req.Header.Set("Content-Type", "application/json")
	rsp, err := cfACLClient.Do(req)
	if err != nil {
		return errCFACL
	}
	defer rsp.Body.Close()
	if rsp.StatusCode != http.StatusOK {
		return errCFACL
	}
	data, err := io.ReadAll(io.LimitReader(rsp.Body, 4097))
	if err != nil || len(data) > 4096 {
		return errCFACL
	}
	var result struct {
		Allowed  bool   `json:"allowed"`
		Contract string `json:"contract"`
	}
	if json.Unmarshal(data, &result) != nil || !result.Allowed || result.Contract != "cf-acl-v1" {
		return errCFACL
	}
	return nil
}

func cfACLPath(repoID, user, value, kind, action string) *appError {
	protected, err := cfACLProtectedNow(repoID)
	if err != nil {
		return &appError{errCFACL, "ACL unavailable", http.StatusServiceUnavailable}
	}
	if !protected {
		return nil
	}
	if err = cfACLAuthorize(context.Background(), repoID, user, []cfACLCheck{{value, kind, action}}); err != nil {
		return &appError{errCFACL, "Current path authorization denied", http.StatusForbidden}
	}
	return nil
}

func cfACLChanges(changes []*diff.DiffEntry) ([]cfACLCheck, error) {
	if len(changes) > 4096 {
		return nil, errCFACL
	}
	checks := make([]cfACLCheck, 0, len(changes))
	for _, change := range changes {
		// Minimal byte upload/overwrite seam. Structural mutations and sync have
		// no source/lifecycle proof here and remain closed for protected repos.
		if change.Status != diff.DiffStatusAdded && change.Status != diff.DiffStatusModified {
			return nil, errCFACL
		}
		value := "/" + change.Name
		if !cfACLCanonical(value) {
			return nil, errCFACL
		}
		checks = append(checks, cfACLCheck{value, "file", "write"})
	}
	return checks, nil
}

// Branch(master) is locked by caller. ACLStore writers take the same lock first.
// The supplied actor is request-authenticated, NEVER Commit.CreatorName.
func cfACLFinal(ctx context.Context, tx *sql.Tx, repoID, oldID, newID, user string) error {
	protected, err := cfACLProtected(ctx, tx, repoID)
	if err != nil {
		return errCFACL
	}
	if !protected {
		return nil
	}
	if user == "" {
		return errCFACL
	}
	oldCommit, err := commitmgr.Load(repoID, oldID)
	if err != nil {
		return errCFACL
	}
	newCommit, err := commitmgr.Load(repoID, newID)
	if err != nil {
		return errCFACL
	}
	var changes []*diff.DiffEntry
	if diff.DiffCommits(oldCommit, newCommit, &changes, false) != nil {
		return errCFACL
	}
	checks, err := cfACLChanges(changes)
	if err != nil {
		return err
	}
	// No-change commits still need current write authority; do not bypass on IDs.
	if len(checks) == 0 {
		return errCFACL
	}
	return cfACLAuthorize(ctx, repoID, user, checks)
}
