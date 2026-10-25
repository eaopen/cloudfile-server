package main

// Optional CF-EDIT: read-only local view + two edit modes, one Guard row
// per checked-out file, no server Intent.
// Native File/Block/Commit/Branch remain authoritative. This endpoint is
// available when the installed Guard table is detected on BOTH native paths.
import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"io"
	"net"
	"net/http"
	"os"
	"path"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/google/uuid"
	"github.com/haiwen/seafile-server/fileserver/blockmgr"
	"github.com/haiwen/seafile-server/fileserver/commitmgr"
	"github.com/haiwen/seafile-server/fileserver/fsmgr"
	"github.com/haiwen/seafile-server/fileserver/option"
	"github.com/haiwen/seafile-server/fileserver/repomgr"
)

const cfEditMaxSize int64 = 128 << 20

type cfEditClaims struct {
	UID      string `json:"uid"`
	RepoID   string `json:"repo_id"`
	FilePath string `json:"path"`
	BaseID   string `json:"file_id"`
	Mode     string `json:"mode"`
	jwt.RegisteredClaims
}
type cfEditProof struct {
	GuardID    string `json:"guard_id"`
	Generation uint64 `json:"generation"`
	Proof      string `json:"proof"`
}
type cfEditRequest struct {
	BaseID       string `json:"base_file_id"`
	TerminalType string `json:"terminal_type"`
	TerminalName string `json:"terminal_name"`
	cfEditProof
}
type cfEditGuard struct {
	ID, Owner, TerminalType, TerminalName, IP, ProofDigest, BaseID string
	Generation                                                     uint64
	Expires                                                        time.Time
}

func cfEditID(s string) bool {
	v, err := uuid.Parse(s)
	return err == nil && v.String() == s
}
func cfEditHex(s string, size int) bool {
	if len(s) != size {
		return false
	}
	for _, b := range []byte(s) {
		if !(b >= '0' && b <= '9' || b >= 'a' && b <= 'f') {
			return false
		}
	}
	return true
}
func cfEditHash(s string) []byte {
	sum := sha256.Sum256([]byte(s))
	return sum[:]
}

// Record only business state changes absent from native CE Activity.
// Sharing the Guard/Branch transaction makes the event truthful without
// an Outbox, duplicate file-commit log or a second audit database.
func cfEditWriteAudit(ctx context.Context, tx *sql.Tx, c *cfEditClaims, operation, commitID string) error {
	var committed interface{}
	if commitID != "" {
		committed = commitID
	}
	_, err := tx.ExecContext(ctx,
		"INSERT INTO cf_audit_event(repo_id,object_type,object_id,operation,operator,source,"+
			"source_path,path_hash,commit_id,result,occurred_at) "+
			"VALUES(?,'file',?, ?,?,'desktop',?,?,?,'success',UTC_TIMESTAMP(6))",
		c.RepoID, c.BaseID, operation, c.Subject, c.FilePath, cfEditHash(c.FilePath), committed)
	return err
}

func cfEditJSON(w http.ResponseWriter, n int, v interface{}) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(n)
	_ = json.NewEncoder(w).Encode(v)
}
func cfEditFail(w http.ResponseWriter, n int, msg string) {
	cfEditJSON(w, n, map[string]string{"code": msg})
}
func cfEditClaimsFor(r *http.Request) (*cfEditClaims, error) {
	pair := strings.Fields(r.Header.Get("Authorization"))
	if len(pair) != 2 || pair[0] != "Bearer" || len(pair[1]) > 8192 {
		return nil, errors.New("bearer required")
	}
	c := new(cfEditClaims)
	t, err := jwt.NewParser(jwt.WithValidMethods([]string{"HS256"}), jwt.WithIssuer("cloudfile-v2-edit"), jwt.WithAudience("cloudfile-edit-agent")).ParseWithClaims(
		pair[1], c, func(*jwt.Token) (interface{}, error) { return []byte(option.JWTPrivateKey), nil })
	if err != nil || t == nil || !t.Valid || c.ExpiresAt == nil || c.IssuedAt == nil ||
		c.ExpiresAt.Time.Sub(c.IssuedAt.Time) > 24*time.Hour ||
		c.IssuedAt.Time.After(time.Now().Add(30*time.Second)) ||
		c.Subject == "" || len(c.Subject) > 255 || c.UID == "" || len(c.UID) > 225 || !cfEditID(c.RepoID) ||
		!cfACLCanonical(c.FilePath) || c.FilePath == "/" || !cfEditHex(c.BaseID, 40) ||
		(c.Mode != "local-view" && c.Mode != "optimistic-edit" && c.Mode != "exclusive-edit") {
		return nil, errors.New("invalid scoped edit ticket")
	}
	return c, nil
}

// Every refusal here is mapped to one HTTP code by the handler, so the reason
// has to travel with the error: an unsupported repo, an unavailable authority
// and an actual denial need different answers from the caller.
var (
	errEditRepoMissing = errors.New("missing repo")
	errEditUnsupported = errors.New("unsupported repo")
	errEditACLUnavail  = errors.New("acl unavailable")
	errEditACLDenied   = errors.New("current path authorization denied")
	errEditDenied      = errors.New("current permission denied")
)

func cfEditAuthorize(c *cfEditClaims, write bool) (*repomgr.Repo, error) {
	repo := repomgr.Get(c.RepoID)
	if repo == nil {
		return nil, errEditRepoMissing
	}
	if repo.VirtualInfo != nil || repo.IsEncrypted || repo.Version != 1 {
		return nil, errEditUnsupported
	}
	action, perm := "read", "download"
	if write {
		action, perm = "write", "upload"
	}
	// Modified 2026-10-23: a managed library is authorized by the V2 path ACL
	// alone, exactly like the ordinary read/write paths in fileop.go; only an
	// unprotected library falls back to the legacy whole-repo permission. The
	// previous `checkPermission(...) || cfACLPath(...)` required BOTH, so a user
	// who is granted rw by the ACL but has no Seafile share row was refused with
	// 403 PERMISSION_DENIED and 本地查看 died at the native read.
	protected, err := cfACLProtectedNow(repo.ID)
	if err != nil {
		return nil, errEditACLUnavail
	}
	if protected {
		if denied := cfACLPath(repo.ID, c.Subject, c.FilePath, "file", action); denied != nil {
			if denied.Code == http.StatusServiceUnavailable {
				return nil, errEditACLUnavail
			}
			return nil, errEditACLDenied
		}
		return repo, nil
	}
	if checkPermission(repo.ID, c.Subject, perm, true) != nil {
		return nil, errEditDenied
	}
	return repo, nil
}
func cfEditBody(r *http.Request) (cfEditRequest, error) {
	var x cfEditRequest
	if r.ContentLength > 4096 {
		return x, errors.New("invalid body")
	}
	d := json.NewDecoder(io.LimitReader(r.Body, 4097))
	d.DisallowUnknownFields()
	if err := d.Decode(&x); err != nil {
		return x, err
	}
	var trailing interface{}
	if d.Decode(&trailing) != io.EOF {
		return x, errors.New("trailing json")
	}
	return x, nil
}
func cfEditHead(ctx context.Context, tx *sql.Tx, id string) (string, error) {
	var head string
	err := tx.QueryRowContext(ctx, "SELECT commit_id FROM Branch WHERE name='master' AND repo_id=? FOR UPDATE", id).Scan(&head)
	return head, err
}
func cfEditActual(repo *repomgr.Repo, head, path string) (string, *commitmgr.Commit, error) {
	c, err := commitmgr.Load(repo.ID, head)
	if err != nil {
		return "", nil, err
	}
	id, mode, err := fsmgr.GetObjIDByPath(repo.StoreID, c.RootID, path)
	if err != nil || !fsmgr.IsRegular(mode) || id == "" {
		return "", nil, errors.New("file missing")
	}
	return id, c, nil
}
func cfEditRow(ctx context.Context, tx *sql.Tx, c *cfEditClaims) (cfEditGuard, error) {
	var g cfEditGuard
	var id, owner, terminalType, terminalName, ip, digest, base sql.NullString
	var exp sql.NullTime
	err := tx.QueryRowContext(ctx,
		"SELECT guard_id,generation,owner_native_user,terminal_type,terminal_name,checkout_ip,proof_digest,base_file_id,lease_expires_at "+
			"FROM cf_edit_guard WHERE repo_id=? AND path_hash=? FOR UPDATE", c.RepoID, cfEditHash(c.FilePath)).
		Scan(&id, &g.Generation, &owner, &terminalType, &terminalName, &ip, &digest, &base, &exp)
	g.ID, g.Owner, g.TerminalType, g.TerminalName, g.IP = id.String, owner.String, terminalType.String, terminalName.String, ip.String
	g.ProofDigest, g.BaseID, g.Expires = digest.String, base.String, exp.Time
	return g, err
}
func cfEditVerified(c *cfEditClaims, g cfEditGuard, p cfEditProof, expiredAllowed bool) bool {
	if g.ID == "" || g.ID != p.GuardID || g.Owner != c.Subject || g.Generation != p.Generation ||
		!cfEditHex(p.Proof, 64) || (!expiredAllowed && !g.Expires.After(time.Now().UTC())) {
		return false
	}
	digest, err := hex.DecodeString(g.ProofDigest)
	if err != nil || len(digest) != 32 {
		return false
	}
	sum := sha256.Sum256([]byte(p.Proof))
	return subtle.ConstantTimeCompare(sum[:], digest) == 1
}

// cfEditResumable lets the owner renew a lapsed lease with a fresh browser
// ticket. Checkout always rotates guard_id, so a still-verified Guard means no
// one else checked out; an unchanged file means no optimistic save intervened.
func cfEditResumable(g cfEditGuard, current string, now time.Time) bool {
	return g.Expires.After(now) || (g.BaseID != "" && current == g.BaseID)
}
func cfEditCurrentPermission(ctx context.Context, tx *sql.Tx, c *cfEditClaims) error {
	protected, err := cfACLProtected(ctx, tx, c.RepoID)
	if err != nil {
		return err
	}
	if protected {
		return cfACLAuthorize(ctx, c.RepoID, c.Subject, []cfACLCheck{{Path: c.FilePath, Kind: "file", Action: "write"}})
	}
	return nil
}
func cfEditRead(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo) {
	if r.Method != "GET" {
		cfEditFail(w, 405, "METHOD")
		return
	}
	id, _, err := cfEditActual(repo, repo.HeadCommitID, c.FilePath)
	if err != nil || id != c.BaseID {
		cfEditFail(w, 409, "VERSION_CHANGED")
		return
	}
	f, err := fsmgr.GetSeafile(repo.StoreID, id)
	if err != nil || f.FileSize > uint64(cfEditMaxSize) {
		cfEditFail(w, 503, "FILE_UNAVAILABLE")
		return
	}
	tmp, err := os.CreateTemp("", "cf-edit-read-*")
	if err != nil {
		cfEditFail(w, 503, "IO_ERROR")
		return
	}
	defer os.Remove(tmp.Name())
	defer tmp.Close()
	for _, block := range f.BlkIDs {
		if !cfEditHex(block, 40) || blockmgr.Read(repo.StoreID, block, tmp) != nil {
			cfEditFail(w, 503, "BLOCK_UNAVAILABLE")
			return
		}
	}
	n, err := tmp.Seek(0, io.SeekCurrent)
	if err != nil || uint64(n) != f.FileSize {
		cfEditFail(w, 503, "BLOCK_MISMATCH")
		return
	}
	if _, err = tmp.Seek(0, io.SeekStart); err != nil {
		cfEditFail(w, 503, "IO_ERROR")
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.FormatInt(n, 10))
	w.Header().Set("X-CF-File-ID", id)
	w.Header().Set("X-CF-Repo-ID", c.RepoID)
	w.Header().Set("X-CF-Path-B64", base64.RawURLEncoding.EncodeToString([]byte(c.FilePath)))
	w.Header().Set("X-CF-Mode", c.Mode)
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(200)
	_, _ = io.Copy(w, tmp)
}

// cfEditPeerIP trusts proxy metadata ONLY from a loopback connection.
// Direct clients cannot spoof a public IP by supplying X-Real-IP.
func cfEditPeerIP(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return ""
	}
	peer := net.ParseIP(host)
	if peer == nil {
		return ""
	}
	if peer.IsLoopback() {
		forwarded := net.ParseIP(strings.TrimSpace(r.Header.Get("X-Real-IP")))
		if forwarded != nil {
			return forwarded.String()
		}
	}
	return peer.String()
}

func cfEditCheckout(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo) {
	if r.Method != "POST" || c.Mode != "exclusive-edit" {
		cfEditFail(w, 405, "METHOD")
		return
	}
	p, err := cfEditBody(r)
	if err != nil || p.BaseID != c.BaseID || len(p.TerminalName) > 128 ||
		(p.TerminalType != "windows" && p.TerminalType != "macos" && p.TerminalType != "linux" && p.TerminalType != "other") {
		cfEditFail(w, 400, "INVALID_CHECKOUT")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()
	tx, err := seafileDB.BeginTx(ctx, nil)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	defer tx.Rollback()
	head, err := cfEditHead(ctx, tx, repo.ID)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	current, _, err := cfEditActual(repo, head, c.FilePath)
	if err != nil || current != c.BaseID {
		cfEditFail(w, 409, "VERSION_CHANGED")
		return
	}
	_, err = tx.ExecContext(ctx,
		"INSERT INTO cf_edit_guard(repo_id,path_hash,path) VALUES(?,?,?) ON DUPLICATE KEY UPDATE repo_id=repo_id",
		repo.ID, cfEditHash(c.FilePath), c.FilePath)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	g, err := cfEditRow(ctx, tx, c)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	if g.ID != "" && g.Expires.After(time.Now().UTC()) {
		cfEditJSON(w, 409, map[string]interface{}{"code": "CHECKED_OUT", "owner": g.Owner, "expires_at": g.Expires.UTC().Format(time.RFC3339)})
		return
	}
	if g.Generation == ^uint64(0) {
		cfEditFail(w, 503, "GENERATION_EXHAUSTED")
		return
	}
	var key [32]byte
	if _, err = rand.Read(key[:]); err != nil {
		cfEditFail(w, 503, "IO_ERROR")
		return
	}
	proof := hex.EncodeToString(key[:])
	guardID := uuid.New().String()
	ip := cfEditPeerIP(r)
	_, err = tx.ExecContext(ctx,
		"UPDATE cf_edit_guard SET guard_id=?,generation=?,owner_uid=?,owner_native_user=?,"+
			"terminal_type=?,terminal_name=?,checkout_ip=?,proof_digest=?,base_file_id=?,"+
			"lease_expires_at=TIMESTAMPADD(SECOND,600,UTC_TIMESTAMP(6)),created_at=UTC_TIMESTAMP(6),updated_at=UTC_TIMESTAMP(6) "+
			"WHERE repo_id=? AND path_hash=?",
		guardID, g.Generation+1, c.UID, c.Subject, p.TerminalType, p.TerminalName, ip,
		hex.EncodeToString(cfEditHash(proof)), current, repo.ID, cfEditHash(c.FilePath))
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	if err = cfEditWriteAudit(ctx, tx, c, "checkout", ""); err != nil || tx.Commit() != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	cfEditJSON(w, 200, map[string]interface{}{"guard_id": guardID, "generation": g.Generation + 1,
		"proof": proof, "base_file_id": current, "expires_in": 600})
}
func cfEditGuardAction(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo, action string) {
	if r.Method != "POST" || c.Mode != "exclusive-edit" {
		cfEditFail(w, 405, "METHOD")
		return
	}
	p, err := cfEditBody(r)
	if err != nil {
		cfEditFail(w, 400, "BAD_PROOF")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 20*time.Second)
	defer cancel()
	tx, err := seafileDB.BeginTx(ctx, nil)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	defer tx.Rollback()
	head, err := cfEditHead(ctx, tx, repo.ID)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	g, err := cfEditRow(ctx, tx, c)
	if err != nil || !cfEditVerified(c, g, p.cfEditProof, true) {
		cfEditFail(w, 409, "STALE_GUARD")
		return
	}
	if action == "renew" {
		now, current := time.Now().UTC(), ""
		if !g.Expires.After(now) {
			if current, _, err = cfEditActual(repo, head, c.FilePath); err != nil {
				cfEditFail(w, 409, "STALE_GUARD")
				return
			}
		}
		if !cfEditResumable(g, current, now) {
			cfEditFail(w, 409, "STALE_GUARD")
			return
		}
	}
	if action == "renew" {
		_, err = tx.ExecContext(ctx, "UPDATE cf_edit_guard SET lease_expires_at=TIMESTAMPADD(SECOND,600,UTC_TIMESTAMP(6)),updated_at=UTC_TIMESTAMP(6) WHERE repo_id=? AND path_hash=?", repo.ID, cfEditHash(c.FilePath))
	} else {
		_, err = tx.ExecContext(ctx, "UPDATE cf_edit_guard SET guard_id=NULL,owner_uid=NULL,owner_native_user=NULL,terminal_type=NULL,terminal_name=NULL,checkout_ip=NULL,proof_digest=NULL,base_file_id=NULL,lease_expires_at=NULL,updated_at=UTC_TIMESTAMP(6) WHERE repo_id=? AND path_hash=?", repo.ID, cfEditHash(c.FilePath))
	}
	if err == nil && action == "release" {
		err = cfEditWriteAudit(ctx, tx, c, "checkout_cancel", "")
	}
	if err != nil || tx.Commit() != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	cfEditJSON(w, 200, map[string]interface{}{"ok": true, "action": action})
}
func cfEditStatus(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo) {
	if r.Method != "GET" {
		cfEditFail(w, 405, "METHOD")
		return
	}
	if option.CheckoutEnabledForRepo(repo.ID) && !option.V2GuardAvailable {
		// Unknown is not "unlocked": recovery must not decide from a false negative.
		cfEditFail(w, 503, "GUARD_UNAVAILABLE")
		return
	}
	id, _, err := cfEditActual(repo, repo.HeadCommitID, c.FilePath)
	if err != nil {
		cfEditFail(w, 404, "FILE_MISSING")
		return
	}
	out := map[string]interface{}{"file_id": id, "head_commit_id": repo.HeadCommitID, "checked_out": false}
	// Recovery may need to confirm CONTENT even if another native commit advanced
	// the File ID. Stream-hash only on explicit status, never on ordinary reads.
	nativeFile, e := fsmgr.GetSeafile(repo.StoreID, id)
	if e != nil || nativeFile.FileSize > uint64(cfEditMaxSize) {
		cfEditFail(w, 503, "STATUS_UNAVAILABLE")
		return
	}
	hasher := sha256.New()
	for _, block := range nativeFile.BlkIDs {
		if !cfEditHex(block, 40) || blockmgr.Read(repo.StoreID, block, hasher) != nil {
			cfEditFail(w, 503, "STATUS_UNAVAILABLE")
			return
		}
	}
	out["sha256"] = hex.EncodeToString(hasher.Sum(nil))
	if option.CheckoutEnabledForRepo(repo.ID) && option.V2GuardAvailable {
		// Checkout is explicitly enabled and its Guard is installed.
		// Disabled Checkout leaves ordinary optimistic editing independent of Guard SQL.
		var gID, owner sql.NullString
		var generation uint64
		var expires sql.NullTime
		err = seafileDB.QueryRowContext(r.Context(),
			"SELECT guard_id,generation,owner_native_user,lease_expires_at FROM cf_edit_guard WHERE repo_id=? AND path_hash=?",
			repo.ID, cfEditHash(c.FilePath)).Scan(&gID, &generation, &owner, &expires)
		if err != nil && err != sql.ErrNoRows {
			cfEditFail(w, 503, "SQL_UNAVAILABLE")
			return
		}
		if err == nil && gID.Valid && expires.Valid && expires.Time.After(time.Now().UTC()) {
			out["checked_out"] = true
			if owner.String == c.Subject {
				out["guard_id"] = gID.String
				out["generation"] = generation
			}
		}
	}
	cfEditJSON(w, 200, out)
}
func cfEditPublish(w http.ResponseWriter, r *http.Request, c *cfEditClaims, repo *repomgr.Repo) {
	if r.Method != "POST" || r.ContentLength < 0 || r.ContentLength > cfEditMaxSize {
		cfEditFail(w, 400, "INVALID_LENGTH")
		return
	}
	action := r.Header.Get("X-CF-Action")
	if action != "optimistic" && action != "commit" && action != "checkin" {
		cfEditFail(w, 400, "ACTION")
		return
	}
	if (c.Mode == "optimistic-edit") != (action == "optimistic") {
		cfEditFail(w, 403, "MODE")
		return
	}
	base := r.Header.Get("X-CF-Base-ID")
	if !cfEditHex(base, 40) {
		cfEditFail(w, 400, "BASE")
		return
	}
	proof := cfEditProof{GuardID: r.Header.Get("X-CF-Guard-ID"), Proof: r.Header.Get("X-CF-Guard-Proof")}
	if action != "optimistic" {
		proof.Generation, _ = strconv.ParseUint(r.Header.Get("X-CF-Guard-Generation"), 10, 64)
		if proof.Generation == 0 {
			cfEditFail(w, 400, "PROOF")
			return
		}
	}
	expectedDigest := r.Header.Get("X-CF-SHA256")
	if !cfEditHex(expectedDigest, 64) {
		cfEditFail(w, 400, "DIGEST")
		return
	}
	tmp, err := os.CreateTemp("", "cf-edit-stage-*")
	if err != nil {
		cfEditFail(w, 503, "IO_ERROR")
		return
	}
	defer os.Remove(tmp.Name())
	defer tmp.Close()
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(tmp, h), io.LimitReader(r.Body, cfEditMaxSize+1))
	if err != nil || n != r.ContentLength || hex.EncodeToString(h.Sum(nil)) != expectedDigest {
		cfEditFail(w, 400, "CONTENT_MISMATCH")
		return
	}
	if err = tmp.Sync(); err != nil {
		cfEditFail(w, 503, "IO_ERROR")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), 90*time.Second)
	defer cancel()
	fileID, size, err := indexBlocks(ctx, repo.StoreID, repo.Version, tmp.Name(), nil, nil)
	if err != nil || !cfEditHex(fileID, 40) || size != n {
		cfEditFail(w, 503, "INDEX_FAILED")
		return
	}
	tx, err := seafileDB.BeginTx(ctx, nil)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	defer tx.Rollback()
	head, err := cfEditHead(ctx, tx, repo.ID)
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	old, headCommit, err := cfEditActual(repo, head, c.FilePath)
	if err != nil || old != base {
		cfEditFail(w, 409, "VERSION_CHANGED")
		return
	}
	if action == "optimistic" {
		// No persistent lock or Intent is allocated for an ordinary save.
		// The Guard query is needed only on deployments with an installed Guard.
		if option.CheckoutEnabledForRepo(repo.ID) && option.V2GuardAvailable {
			var held int
			err = tx.QueryRowContext(ctx, "SELECT COUNT(*) FROM cf_edit_guard WHERE repo_id=? AND path_hash=? AND guard_id IS NOT NULL AND lease_expires_at>UTC_TIMESTAMP(6) FOR UPDATE", repo.ID, cfEditHash(c.FilePath)).Scan(&held)
			if err != nil || held != 0 {
				cfEditFail(w, 409, "CHECKED_OUT")
				return
			}
		}
	} else {
		g, e := cfEditRow(ctx, tx, c)
		if e != nil || g.BaseID != base || !cfEditVerified(c, g, proof, false) {
			cfEditFail(w, 409, "STALE_GUARD")
			return
		}
	}
	if err = cfEditCurrentPermission(ctx, tx, c); err != nil {
		cfEditFail(w, 403, "PERMISSION_CHANGED")
		return
	}
	newCommitID := head
	if fileID != old {
		dent := fsmgr.NewDirent(fileID, path.Base(c.FilePath), syscall.S_IFREG|0644, time.Now().Unix(), c.Subject, size)
		root, err := doPutFile(repo, headCommit.RootID, path.Dir(c.FilePath), dent)
		if err != nil || root == "" {
			cfEditFail(w, 503, "NATIVE_FS")
			return
		}
		cm := commitmgr.NewCommit(repo.ID, head, root, c.Subject, "Modified "+path.Base(c.FilePath))
		repomgr.RepoToCommit(repo, cm)
		if err = commitmgr.Save(cm); err != nil {
			cfEditFail(w, 503, "NATIVE_COMMIT")
			return
		}
		newCommitID = cm.CommitID
	}
	if action == "checkin" {
		_, err = tx.ExecContext(ctx, "UPDATE cf_edit_guard SET guard_id=NULL,owner_uid=NULL,owner_native_user=NULL,terminal_type=NULL,terminal_name=NULL,checkout_ip=NULL,proof_digest=NULL,base_file_id=NULL,lease_expires_at=NULL,updated_at=UTC_TIMESTAMP(6) WHERE repo_id=? AND path_hash=? AND guard_id=?", repo.ID, cfEditHash(c.FilePath), proof.GuardID)
	} else if action == "commit" {
		_, err = tx.ExecContext(ctx, "UPDATE cf_edit_guard SET base_file_id=?,updated_at=UTC_TIMESTAMP(6) WHERE repo_id=? AND path_hash=? AND guard_id=?", fileID, repo.ID, cfEditHash(c.FilePath), proof.GuardID)
	}
	if err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	if head != newCommitID {
		_, err = tx.ExecContext(ctx, "UPDATE Branch SET commit_id=? WHERE repo_id=? AND name='master' AND commit_id=?", newCommitID, repo.ID, head)
		if err != nil {
			cfEditFail(w, 503, "BRANCH_FAILED")
			return
		}
	}
	if action == "checkin" {
		if err = cfEditWriteAudit(ctx, tx, c, "checkin", newCommitID); err != nil {
			cfEditFail(w, 503, "SQL_UNAVAILABLE")
			return
		}
	}
	if err = tx.Commit(); err != nil {
		cfEditFail(w, 503, "SQL_UNAVAILABLE")
		return
	}
	if head != newCommitID {
		_ = onBranchUpdated(repo.ID, newCommitID, true)
		go mergeVirtualRepoPool.AddTask(repo.ID)
		go updateSizePool.AddTask(repo.ID)
	}
	cfEditJSON(w, 200, map[string]interface{}{"state": "published", "file_id": fileID,
		"result_commit_id": newCommitID, "checked_in": action == "checkin"})
}

// Without Guard storage, the ordinary Agent path (read, status, optimistic
// CAS publish) remains usable. Only exclusive operations require Checkout.
// A local-view JWT is a read grant only, regardless of whether the account
// ALSO happens to have write permission. Do not let it reach native Guard or
// publish code even on installations that have a Checkout table.
func cfEditReadOnlyRoute(mode, route string) bool {
	return mode != "local-view" || route == "read"
}

// Without Guard storage, local-view only reads and optimistic-edit can
// operate normally; neither creates a Checkout row.
func cfEditAllowedWithoutCheckout(mode, route, action string) bool {
	if mode == "local-view" {
		return route == "read"
	}
	if mode != "optimistic-edit" {
		return false
	}
	return route == "read" || route == "status" ||
		(route == "publish" && action == "optimistic")
}

// Let the caller tell the three refusals apart without leaking policy detail.
func cfEditAuthFail(w http.ResponseWriter, err error) {
	switch {
	case errors.Is(err, errEditRepoMissing):
		cfEditFail(w, 404, "REPO_MISSING")
	case errors.Is(err, errEditUnsupported):
		cfEditFail(w, 409, "UNSUPPORTED_REPO")
	case errors.Is(err, errEditACLUnavail):
		cfEditFail(w, 503, "ACL_UNAVAILABLE")
	case errors.Is(err, errEditACLDenied):
		cfEditFail(w, 403, "ACL_DENIED")
	default:
		cfEditFail(w, 403, "PERMISSION_DENIED")
	}
}

func cfEditAPI(w http.ResponseWriter, r *http.Request) {
	c, err := cfEditClaimsFor(r)
	if err != nil {
		cfEditFail(w, 401, "AUTH_REQUIRED")
		return
	}
	route := strings.TrimPrefix(r.URL.Path, "/cf-edit/v1/")
	if !cfEditReadOnlyRoute(c.Mode, route) {
		cfEditFail(w, 403, "READ_ONLY_TICKET")
		return
	}
	selected := option.CheckoutEnabledForRepo(c.RepoID)
	if !selected && !cfEditAllowedWithoutCheckout(c.Mode, route, r.Header.Get("X-CF-Action")) {
		cfEditFail(w, 409, "CHECKOUT_UNSUPPORTED")
		return
	}
	// OFF repositories do not depend on the Guard schema. OPTIONAL repositories
	// must refuse all writes when Guard is unavailable, including optimistic edits.
	// Status is refused too: unknown lock state must never look like "no lock",
	// otherwise the Agent could treat a live Guard as absent during recovery.
	if selected && !option.V2GuardAvailable && route != "read" {
		cfEditFail(w, 503, "GUARD_UNAVAILABLE")
		return
	}
	write := route != "read" && route != "status"
	repo, err := cfEditAuthorize(c, write)
	if err != nil {
		cfEditAuthFail(w, err)
		return
	}
	switch route {
	case "read":
		cfEditRead(w, r, c, repo)
	case "checkout":
		cfEditCheckout(w, r, c, repo)
	case "renew":
		cfEditGuardAction(w, r, c, repo, "renew")
	case "release":
		cfEditGuardAction(w, r, c, repo, "release")
	case "status":
		cfEditStatus(w, r, c, repo)
	case "publish":
		cfEditPublish(w, r, c, repo)
	default:
		cfEditFail(w, 404, "UNKNOWN_EDIT_ROUTE")
	}
}
