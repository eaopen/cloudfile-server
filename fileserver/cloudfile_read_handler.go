package main

import (
	"net/http"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/haiwen/seafile-server/fileserver/repomgr"
)

// Deliberately unregistered until identity/logout and deployment gates pass.
// Reuses native file/Range readers, not a second block-transfer implementation.
func cloudFileReadCB(rsp http.ResponseWriter, r *http.Request) *appError {
	if r.Method != "GET" && r.Method != "HEAD" {
		return &appError{nil, "Read requires GET or HEAD", http.StatusMethodNotAllowed}
	}
	if r.TLS == nil {
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

type cloudFileTrackedResponse struct {
	http.ResponseWriter
	committed bool
}

func (w *cloudFileTrackedResponse) WriteHeader(status int) {
	w.committed = true
	w.ResponseWriter.WriteHeader(status)
}

func (w *cloudFileTrackedResponse) Write(data []byte) (int, error) {
	w.committed = true
	return w.ResponseWriter.Write(data)
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
	w.ResponseWriter.WriteHeader(status)
}
