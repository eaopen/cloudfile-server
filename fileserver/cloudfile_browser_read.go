package main

import (
	"net/http"
	"strings"
)

// Browser navigation cannot set Bearer headers. This URL is the same short-lived
// one-use read ticket, consumed only by the guarded reader, never the legacy API.
func cloudFileBrowserReadCB(rsp http.ResponseWriter, r *http.Request) *appError {
	cloudFileReadSecurityHeaders(rsp.Header())
	parts := strings.Split(strings.TrimPrefix(r.URL.Path, "/cloudfile/files/"), "/")
	if !strings.HasPrefix(r.URL.Path, "/cloudfile/files/") || len(parts) != 2 ||
		!canonicalTicketUUID(parts[0]) || parts[1] == "" ||
		len(r.Header.Values("Authorization")) != 0 || len(r.Header.Values("X-CloudFile-Filename")) != 0 {
		return &appError{nil, "Invalid browser read request", http.StatusBadRequest}
	}
	request := r.Clone(r.Context())
	// Browser ambient cookies never authorize this endpoint. The URL's ticket
	// alone supplies the native account and proof rechecked on every byte chunk.
	request.Header.Del("Cookie")
	request.Header.Set("Authorization", "Bearer "+parts[0])
	request.Header.Set("X-CloudFile-Filename", parts[1])
	return cloudFileReadCB(rsp, request)
}
