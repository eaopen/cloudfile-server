package main

// CF-ACL is independent of CF-EDIT. Hub owns policy semantics; native code
// supplies authenticated actors and actual paths, then rechecks at publication.
import (
	"bytes"
	"context"
	"crypto/sha256"
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

var (
	errCFACL            = errors.New("CF-ACL: access denied or invalid scope")
	errCFACLUnavailable = errors.New("CF-ACL: current authority unavailable")
)

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
	if err != nil {
		return &appError{errCFACLUnavailable, "ACL unavailable", http.StatusServiceUnavailable}
	}
	if protected {
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
		return errCFACLUnavailable
	}
	token, err := utils.GenSeahubJWTToken()
	if err != nil {
		return errCFACLUnavailable
	}
	req, err := http.NewRequestWithContext(ctx, "POST", strings.TrimRight(option.SeahubURL, "/")+"/cloudfile/acl/check/", bytes.NewReader(body))
	if err != nil {
		return errCFACLUnavailable
	}
	req.Header.Set("Authorization", "Token "+token)
	req.Header.Set("Content-Type", "application/json")
	rsp, err := cfACLClient.Do(req)
	if err != nil {
		return errCFACLUnavailable
	}
	defer rsp.Body.Close()
	if rsp.StatusCode != http.StatusOK {
		if rsp.StatusCode >= 400 && rsp.StatusCode < 500 && rsp.StatusCode != http.StatusTooManyRequests {
			return errCFACL
		}
		return errCFACLUnavailable
	}
	data, err := io.ReadAll(io.LimitReader(rsp.Body, 4097))
	if err != nil || len(data) > 4096 {
		return errCFACLUnavailable
	}
	var result struct {
		Allowed  bool   `json:"allowed"`
		Contract string `json:"contract"`
	}
	if json.Unmarshal(data, &result) != nil || result.Contract != "cf-acl-v1" {
		return errCFACLUnavailable
	}
	if !result.Allowed {
		return errCFACL
	}
	return nil
}

// A policy denial and a policy-service outage both refuse access, but callers
// must see 403 versus 503 respectively. Never downgrade an unavailable
// authority into a permission decision or allow via the CE fallback.
func cfACLPathFailure(err error) *appError {
	if errors.Is(err, errCFACLUnavailable) {
		return &appError{errCFACLUnavailable, "ACL unavailable", http.StatusServiceUnavailable}
	}
	return &appError{errCFACL, "Current path authorization denied", http.StatusForbidden}
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
		return cfACLPathFailure(err)
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

// Bounded optimistic SQL proof of the exact ACL row/revision set. The Hub's
// reviewed ACL writers acquire Branch(master) before changing these rows.
// The same digest taken with current locking reads in Branch's final short
// transaction therefore detects intervening ACL adds/removes/edits.
// No additional table, global policy epoch or cross-request decision cache.
type cfACLProof struct {
	Hash      [32]byte
	Protected bool
}

type cfACLProofDB interface {
	QueryContext(context.Context, string, ...interface{}) (*sql.Rows, error)
	QueryRowContext(context.Context, string, ...interface{}) *sql.Row
}

func cfACLCurrentProof(ctx context.Context, db cfACLProofDB, repoID string, locked bool) (cfACLProof, error) {
	var out cfACLProof
	if db == nil || len(repoID) != 36 {
		return out, errCFACLUnavailable
	}
	var origin string
	err := db.QueryRowContext(ctx, "SELECT origin_repo FROM VirtualRepo WHERE repo_id=?", repoID).Scan(&origin)
	if err != nil && err != sql.ErrNoRows {
		return out, errCFACLUnavailable
	}
	scopes := []string{repoID}
	if err == nil {
		if len(origin) != 36 || origin == repoID {
			return out, errCFACLUnavailable
		}
		scopes = append(scopes, origin)
	}
	hasher := sha256.New()
	query := "SELECT HEX(path_hash),subject_type,provider,namespace,external_id,revision " +
		"FROM cf_dir_acl FORCE INDEX(PRIMARY) WHERE repo_id=? " +
		"ORDER BY path_hash,subject_type,provider,namespace,external_id LIMIT 4097"
	if locked {
		query += " FOR UPDATE"
	}
	for _, scope := range scopes {
		_, _ = io.WriteString(hasher, scope)
		_, _ = hasher.Write([]byte{0})
		rows, err := db.QueryContext(ctx, query, scope)
		if err != nil {
			return out, errCFACLUnavailable
		}
		n := 0
		for rows.Next() {
			var cols [6]string
			if err := rows.Scan(&cols[0], &cols[1], &cols[2], &cols[3], &cols[4], &cols[5]); err != nil {
				rows.Close()
				return out, errCFACLUnavailable
			}
			n++
			if n > 4096 {
				rows.Close()
				return out, errCFACLUnavailable
			}
			for _, value := range cols {
				_, _ = io.WriteString(hasher, value)
				_, _ = hasher.Write([]byte{0})
			}
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return out, errCFACLUnavailable
		}
		if n != 0 {
			out.Protected = true
		}
	}
	copy(out.Hash[:], hasher.Sum(nil))
	return out, nil
}

// Phase A runs before BeginTx: current protected-scope check, one diff and one
// Hub write authorization with authenticated request actor. No Branch lock.
func cfACLPreflight(ctx context.Context, repoID, oldID, newID, user string) (cfACLProof, error) {
	proof, err := cfACLCurrentProof(ctx, seafileDB, repoID, false)
	if err != nil {
		return proof, err
	}
	if !proof.Protected {
		return proof, nil
	}
	if user == "" {
		return proof, errCFACL
	}
	oldCommit, err := commitmgr.Load(repoID, oldID)
	if err != nil {
		return proof, errCFACL
	}
	newCommit, err := commitmgr.Load(repoID, newID)
	if err != nil {
		return proof, errCFACL
	}
	var changes []*diff.DiffEntry
	if diff.DiffCommits(oldCommit, newCommit, &changes, false) != nil {
		return proof, errCFACL
	}
	checks, err := cfACLChanges(changes)
	if err != nil || len(checks) == 0 {
		return proof, errCFACL
	}
	return proof, cfACLAuthorize(ctx, repoID, user, checks)
}

// Phase B runs under Branch(master) after its HEAD CAS read. There is no
// remote RPC, commit diff, or repeated policy evaluation under the lock.
// A changed ACL set invalidates preflight; the client must re-authorize.
func cfACLFinal(ctx context.Context, tx *sql.Tx, repoID string, expected cfACLProof) error {
	current, err := cfACLCurrentProof(ctx, tx, repoID, true)
	if err != nil {
		return err
	}
	if current.Hash != expected.Hash {
		return errCFACL
	}
	return nil
}
