package main

import (
	"context"
	"net"
	"net/http"
	"strconv"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/google/uuid"

	"github.com/haiwen/seafile-server/fileserver/option"
	"github.com/haiwen/seafile-server/fileserver/repomgr"
)

// Deliberately unregistered until identity/logout and deployment gates pass.
// Reuses native file/Range readers, not a second block-transfer implementation.
func cloudFileReadCB(rsp http.ResponseWriter, r *http.Request) (returned *appError) {
	cloudFileReadSecurityHeaders(rsp.Header())
	defer cloudFileReadSecurityHeaders(rsp.Header())
	if r.Method != "GET" && r.Method != "HEAD" {
		return &appError{nil, "Read requires GET or HEAD", http.StatusMethodNotAllowed}
	}
	if !cloudFileSecureTransport(r, option.CloudFileTrustedTLSProxies) {
		return &appError{nil, "Secure read transport required", http.StatusUnauthorized}
	}
	if r.URL.RawQuery != "" || len(r.Header.Values("Cookie")) != 0 ||
		r.ContentLength != 0 || len(r.TransferEncoding) != 0 {
		return &appError{nil, "Invalid read request", http.StatusBadRequest}
	}
	auth := r.Header.Values("Authorization")
	if len(auth) != 1 || !strings.HasPrefix(auth[0], "Bearer ") {
		return &appError{nil, "Read ticket required", http.StatusUnauthorized}
	}
	token := strings.TrimPrefix(auth[0], "Bearer ")
	filename := r.Header.Get("X-CloudFile-Filename")
	if filename == "" {
		filename = "download"
	}
	if len(r.Header.Values("X-CloudFile-Filename")) > 1 || len(filename) > 255 ||
		!utf8.ValidString(filename) || strings.ContainsAny(filename, "\"/\\") ||
		strings.IndexFunc(filename, unicode.IsControl) >= 0 || len(r.Header.Values("Range")) > 1 {
		return &appError{nil, "Invalid read headers", http.StatusBadRequest}
	}
	tracked := &cloudFileTrackedResponse{ResponseWriter: rsp}
	writer, err := newCloudFileReadWriter(tracked, r.Context(), token)
	if err != nil {
		return &appError{nil, "Read transfer unavailable", http.StatusServiceUnavailable}
	}
	defer writer.Close()
	info, failure := consumeCloudFileReadTicket(token)
	if failure != nil {
		return failure
	}
	requestID := uuid.New().String()
	fact, err := captureCloudFileReadAudit(token, requestID)
	if err != nil || fact.RepoID != info.repoID || fact.Operation != info.op {
		return &appError{nil, "Read audit context unavailable", http.StatusServiceUnavailable}
	}
	rsp.Header().Set("X-Request-ID", requestID)
	fact.Outcome = cloudFileReadOutcome{Result: "attempted"}
	if appendCloudFileReadAudit(r.Context(), seafileDB, fact) != nil {
		return &appError{nil, "Read audit unavailable", http.StatusServiceUnavailable}
	}
	defer func() {
		interruption := recover()
		// End the native transfer before recording terminal state. The earlier
		// unconditional defer still covers failures before audit setup; Close is
		// idempotent and never retries an uncertain native cleanup.
		cleanupError := writer.Close()
		if cleanupError != nil {
			fact.Reason = "transfer_cleanup_unconfirmed"
		}
		observedFailure := writer.failed
		if returned != nil || interruption != nil {
			observedFailure = errCloudFileReadEnded
		}
		expected, lengthError := strconv.ParseUint(tracked.Header().Get("Content-Length"), 10, 64)
		fact.Outcome = tracked.outcome(r.Method, expected, lengthError == nil,
			observedFailure, r.Context().Err() != nil)
		// Client disconnect must not cancel the server's terminal audit. This
		// independent SQL operation remains bounded by its own five-second limit.
		auditError := appendCloudFileReadAudit(context.Background(), seafileDB, fact)
		if interruption != nil {
			panic(interruption)
		}
		if auditError != nil || cleanupError != nil {
			if tracked.committed {
				panic(http.ErrAbortHandler)
			}
			tracked.Header().Del("Content-Length")
			tracked.Header().Del("Content-Disposition")
			returned = &appError{nil, "Read completion unavailable", http.StatusServiceUnavailable}
		}
	}()
	repo := repomgr.Get(info.repoID)
	if repo == nil || repo.VirtualInfo != nil {
		return &appError{nil, "Read target unavailable", http.StatusForbidden}
	}
	guarded := &cloudFileGuardedResponse{ResponseWriter: tracked, writer: writer}
	guarded.Header().Set("Cache-Control", "no-store, max-age=0")
	guarded.Header().Set("Referrer-Policy", "no-referrer")
	var cryptKey *seafileCrypt
	if repo.IsEncrypted {
		cryptKey, failure = parseCryptKey(guarded, info.repoID, info.user, repo.EncVersion)
		if failure != nil {
			return failure
		}
	}
	if ranges := r.Header.Get("Range"); ranges != "" && !repo.IsEncrypted {
		failure = doFileRange(guarded, r, repo, info.objID, filename, info.op, ranges, info.user)
	} else {
		failure = doFile(guarded, r, repo, info.objID, filename, info.op, cryptKey, info.user)
	}
	if r.Method == "GET" && failure == nil && writer.failed == nil {
		expected, err := strconv.ParseUint(guarded.Header().Get("Content-Length"), 10, 64)
		if err != nil || expected != tracked.bytes {
			writer.failed = errCloudFileReadEnded
		}
	}
	if writer.failed != nil || (failure != nil && tracked.committed) {
		if tracked.committed {
			panic(http.ErrAbortHandler)
		}
		guarded.Header().Del("Content-Length")
		guarded.Header().Del("Content-Disposition")
		return &appError{nil, "Read transfer ended", http.StatusServiceUnavailable}
	}
	return failure
}

func cloudFileSecureTransport(r *http.Request, proxies []*net.IPNet) bool {
	if r.TLS != nil {
		return true
	}
	// Only the actual TCP peer can attest TLS termination. Forwarded client
	// addresses/Host never select trust; proxies must overwrite this header.
	protocol := r.Header.Values("X-Forwarded-Proto")
	if len(protocol) != 1 || protocol[0] != "https" {
		return false
	}
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		return false
	}
	ip := net.ParseIP(host)
	if ip == nil {
		return false
	}
	for _, network := range proxies {
		if network != nil && network.Contains(ip) {
			return true
		}
	}
	return false
}

type cloudFileTrackedResponse struct {
	http.ResponseWriter
	committed bool
	bytes     uint64
	status    int
}

func (w *cloudFileTrackedResponse) WriteHeader(status int) {
	// Match net/http: informational responses do not commit the final status;
	// repeated final WriteHeader calls cannot rewrite the recorded outcome.
	if w.committed {
		return
	}
	cloudFileReadSecurityHeaders(w.Header())
	if status >= 100 && status < 200 && status != http.StatusSwitchingProtocols {
		w.ResponseWriter.WriteHeader(status)
		return
	}
	w.committed = true
	w.status = status
	w.ResponseWriter.WriteHeader(status)
}

func (w *cloudFileTrackedResponse) Write(data []byte) (int, error) {
	cloudFileReadSecurityHeaders(w.Header())
	if !w.committed {
		w.status = http.StatusOK
	}
	w.committed = true
	n, err := w.ResponseWriter.Write(data)
	if n > 0 && n <= len(data) {
		w.bytes += uint64(n)
	}
	return n, err
}

// A server-side observation, not a client receipt or persisted audit event.
// Call only once the native reader has returned. The durable audit adapter
// must derive actor/path from native transfer state, never request headers.
type cloudFileReadOutcome struct {
	Result    string
	Status    int
	BytesSent uint64
}

func (w *cloudFileTrackedResponse) outcome(method string, expected uint64, expectedKnown bool, failure error, cancelled bool) cloudFileReadOutcome {
	result := "failed"
	if cancelled || (failure != nil && w.committed) {
		result = "interrupted"
	} else if failure == nil && w.committed && (w.status == http.StatusOK || w.status == http.StatusPartialContent) {
		if method == http.MethodHead && w.bytes == 0 {
			result = "succeeded"
		} else if method == http.MethodGet && expectedKnown && w.bytes == expected {
			result = "stream_completed"
		} else {
			result = "interrupted"
		}
	}
	return cloudFileReadOutcome{Result: result, Status: w.status, BytesSent: w.bytes}
}

// Native readers also serve legacy public links and set permissive CORS.
// A managed bearer transfer must not inherit that separate trust boundary.
// Cross-origin deployment requires an explicit reviewed proxy/origin policy;
// this handler never reflects Origin or grants wildcard browser access.
func cloudFileReadSecurityHeaders(header http.Header) {
	for key := range header {
		if strings.HasPrefix(strings.ToLower(key), "access-control-") {
			delete(header, key)
		}
	}
	header.Set("Cache-Control", "no-store, max-age=0")
	header.Set("Pragma", "no-cache")
	header.Set("Referrer-Policy", "no-referrer")
	header.Set("X-Content-Type-Options", "nosniff")
}

type cloudFileGuardedResponse struct {
	http.ResponseWriter
	writer *cloudFileReadWriter
}

func (w *cloudFileGuardedResponse) Write(data []byte) (int, error) { return w.writer.Write(data) }
func (w *cloudFileGuardedResponse) WriteHeader(status int) {
	if w.writer.failed != nil {
		return
	}
	if err := w.writer.context.Err(); err != nil {
		w.writer.failed = err
		return
	}
	if err := w.writer.check(); err != nil {
		w.writer.failed = err
		return
	}
	if err := w.writer.context.Err(); err != nil {
		w.writer.failed = err
		return
	}
	w.ResponseWriter.WriteHeader(status)
}
