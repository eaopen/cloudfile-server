// CloudFile write lifecycle seam in the Go fileserver.
//
// The Go fileserver chunks the body, writes blocks and fs objects, generates a
// commit and updates the branch entirely on its own -- it never enters
// repo-op.c, where the C seam lives. It is therefore the one write path the C
// side cannot see, and a capability that only guarded C would leave every
// upload and every sync unguarded.
//
// Rather than reimplement the adjudication in Go, this asks seaf-server over
// RPC, so C stays the single authority. Same shape and the same reasoning as
// cf_ext.go: a second implementation is a second thing to drift.
//
// Where this deliberately differs from cf_ext.go
//
// cf_ext.go caches its answer for five minutes and treats an RPC failure as
// permissive. Both are right for a slow-moving library-level question. Neither
// is right here:
//
//   - Caching "no provider is registered" would leave a window, after an
//     operator enables a capability and restarts seaf-server, in which every
//     upload bypasses it. A security hole that closes by itself after a few
//     minutes is the worst kind to diagnose. So the verdict is never cached;
//     prepare is asked every time.
//
//   - Failing open on an RPC error would do the same thing whenever
//     seaf-server hiccups. So once a provider is known to be registered, an
//     unreachable RPC fails closed, per the third iron law: when the rules
//     cannot be read, refuse.
//
// What IS cached is only which of three worlds we are in, and only to pick the
// failure mode:
//
//	unsupported -- no such RPC, i.e. an upstream seaf-server. Skip entirely;
//	               costs nothing. Re-probed occasionally so that losing the
//	               race with seaf-server's startup heals itself.
//	inactive    -- the RPC exists, no capability registered. Ask anyway (C
//	               answers off one global bool, no query), but fail open if
//	               the RPC is unreachable: there is nothing to enforce, and
//	               breaking uploads on a baseline deployment would be a
//	               regression against stock CE.
//	active      -- a capability is registered. Ask, and fail closed.
//
// Contract: cloudfile-docker/docs/fileop-lifecycle.md

package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"time"

	log "github.com/sirupsen/logrus"
)

// Operation vocabulary. Must stay identical to common/cf-fileop.h; the shared
// case set in cloudfile-docker/docs/fileop-cases.json checks both ends.
const (
	cfOpCreateFile   = "create-file"
	cfOpUpdateFile   = "update-file"
	cfOpDelete       = "delete"
	cfOpMkdir        = "mkdir"
	cfOpRename       = "rename"
	cfOpMove         = "move"
	cfOpCopy         = "copy"
	cfOpRevertFile   = "revert-file"
	cfOpRevertDir    = "revert-dir"
	cfOpRevertRepo   = "revert-repo"
	cfOpUpdateDir    = "update-dir"
	cfOpUploadBlocks = "upload-blocks"
	cfOpSyncUpdate   = "sync-update"
)

// cfFileOp is the wire form of C's CfFileOp. Field names must match
// common/cf-fileop-json.c.
type cfFileOp struct {
	Op string `json:"op"`

	RepoID string   `json:"repo_id,omitempty"`
	Dir    string   `json:"dir,omitempty"`
	Name   string   `json:"name,omitempty"`
	Names  []string `json:"names,omitempty"`

	SrcRepoID string   `json:"src_repo_id,omitempty"`
	SrcDir    string   `json:"src_dir,omitempty"`
	SrcName   string   `json:"src_name,omitempty"`
	SrcNames  []string `json:"src_names,omitempty"`

	User           string `json:"user,omitempty"`
	Client         string `json:"client,omitempty"`
	ExpectCommitID string `json:"expect_commit_id,omitempty"`

	CommitID string `json:"commit_id,omitempty"`
	FileID   string `json:"file_id,omitempty"`
}

type cfVerdict struct {
	Allowed bool   `json:"allowed"`
	Code    int    `json:"code"`
	Message string `json:"message"`
}

// CloudFile's own error codes, mirroring common/cf-fileop.h.
const (
	cfErrFileLocked      = 600
	cfErrVersionMismatch = 601
)

const (
	cfSeamUnknown = iota
	cfSeamUnsupported
	cfSeamInactive
	cfSeamActive
)

// How long an "unsupported" verdict stands before we probe again. Long enough
// that an upstream build pays almost nothing, short enough that starting
// before seaf-server does not disable the seam for the process lifetime.
const cfSeamReprobeSeconds = 300

var (
	cfSeamMu      sync.Mutex
	cfSeamState   = cfSeamUnknown
	cfSeamProbeAt int64
	cfSeamWarned  bool
)

// cfSeam reports which of the three worlds we are in, probing seaf-server when
// the cached answer has expired.
func cfSeam() int {
	cfSeamMu.Lock()
	defer cfSeamMu.Unlock()

	now := time.Now().Unix()
	if cfSeamState != cfSeamUnknown && now < cfSeamProbeAt {
		return cfSeamState
	}

	ret, err := rpcclient.Call("cf_fileop_active")
	if err != nil {
		// No such RPC: an upstream seaf-server, or it is not up yet.
		cfSeamState = cfSeamUnsupported
		cfSeamProbeAt = now + cfSeamReprobeSeconds
		if !cfSeamWarned {
			cfSeamWarned = true
			log.Printf("CloudFile: write lifecycle RPC unavailable (%v); "+
				"treating this server as stock CE and re-probing every %ds",
				err, cfSeamReprobeSeconds)
		}
		return cfSeamState
	}

	cfSeamWarned = false

	active := false
	switch v := ret.(type) {
	case float64:
		active = v != 0
	case int64:
		active = v != 0
	case json.Number:
		n, convErr := v.Int64()
		active = convErr == nil && n != 0
	}

	if active {
		cfSeamState = cfSeamActive
	} else {
		cfSeamState = cfSeamInactive
	}

	// Short-lived on purpose: this only selects the failure mode, and the
	// verdict below is asked fresh every time regardless.
	cfSeamProbeAt = now + 30

	return cfSeamState
}

// cfHTTPStatus maps a provider's refusal code to what the client should see.
func cfHTTPStatus(code int) int {
	switch code {
	case cfErrFileLocked:
		return http.StatusLocked
	case cfErrVersionMismatch:
		return http.StatusConflict
	default:
		return http.StatusForbidden
	}
}

// cfFileOpPrepare asks whether a write may proceed. Returns nil to allow.
//
// The refusal message is passed through to the client verbatim: it is the only
// thing that can explain who holds the lock and for how long, and a refusal
// nobody can act on becomes a support ticket with nothing in it.
func cfFileOpPrepare(fop *cfFileOp) *appError {
	state := cfSeam()
	if state == cfSeamUnsupported {
		return nil
	}

	payload, err := json.Marshal(fop)
	if err != nil {
		// Our own struct failed to marshal: a bug, not a server problem.
		// Refusing is still correct -- we cannot ask, so we cannot allow.
		err := fmt.Errorf("failed to encode file operation: %v", err)
		return &appError{err, "", http.StatusInternalServerError}
	}

	ret, err := rpcclient.Call("cf_fileop_prepare", string(payload))
	if err != nil {
		if state == cfSeamInactive {
			// Nothing is registered, so there is nothing this call could have
			// refused. Letting the write through keeps a baseline deployment
			// behaving exactly like stock CE when seaf-server blips.
			log.Printf("CloudFile: prepare RPC failed with no provider registered, allowing: %v", err)
			return nil
		}
		log.Printf("CloudFile: prepare RPC failed with a provider registered, refusing: %v", err)
		return &appError{err, "The server cannot verify this operation right now.",
			http.StatusServiceUnavailable}
	}

	raw, ok := ret.(string)
	if !ok || raw == "" {
		if state == cfSeamInactive {
			return nil
		}
		err := fmt.Errorf("malformed verdict from cf_fileop_prepare: %v", ret)
		return &appError{err, "", http.StatusServiceUnavailable}
	}

	var verdict cfVerdict
	if err := json.Unmarshal([]byte(raw), &verdict); err != nil {
		if state == cfSeamInactive {
			return nil
		}
		err := fmt.Errorf("failed to decode verdict %q: %v", raw, err)
		return &appError{err, "", http.StatusServiceUnavailable}
	}

	if verdict.Allowed {
		return nil
	}

	return &appError{nil, verdict.Message, cfHTTPStatus(verdict.Code)}
}

// cfFileOpReport sends COMMITTED or ABORTED. Neither can change the outcome,
// so a failure here is logged and swallowed: the write already happened (or
// already failed), and turning a bookkeeping error into a client error would
// make the client retry something that took effect.
func cfFileOpReport(rpcName string, fop *cfFileOp) {
	if cfSeam() != cfSeamActive {
		return
	}

	payload, err := json.Marshal(fop)
	if err != nil {
		log.Printf("CloudFile: failed to encode %s payload: %v", rpcName, err)
		return
	}

	if _, err := rpcclient.Call(rpcName, string(payload)); err != nil {
		log.Printf("CloudFile: %s RPC failed: %v", rpcName, err)
	}
}

func cfFileOpCommitted(fop *cfFileOp) {
	cfFileOpReport("cf_fileop_committed", fop)
}

func cfFileOpAborted(fop *cfFileOp) {
	cfFileOpReport("cf_fileop_aborted", fop)
}
