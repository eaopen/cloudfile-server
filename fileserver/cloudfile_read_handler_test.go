package main

import (
	"context"
	"crypto/tls"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestCloudFileReadRequestRejectsBeforeRPC(t *testing.T) {
	for _, fixture := range []struct {
		change func(*http.Request)
		status int
	}{
		{func(r *http.Request) { r.Method = "POST" }, 405},
		{func(r *http.Request) { r.TLS = nil }, 401},
		{func(r *http.Request) { r.URL.RawQuery = "ticket=private" }, 400},
		{func(r *http.Request) { r.Header.Set("Cookie", "sessionid=private") }, 400},
		{func(r *http.Request) { r.ContentLength = 1 }, 400},
		{func(r *http.Request) { r.Header.Del("Authorization") }, 401},
		{func(r *http.Request) { r.Header.Add("Authorization", "Bearer duplicate") }, 401},
		{func(r *http.Request) { r.Header.Set("X-CloudFile-Filename", "bad\"name") }, 400},
		{func(r *http.Request) { r.Header.Add("Range", "bytes=0-1"); r.Header.Add("Range", "bytes=2-3") }, 400},
	} {
		r := httptest.NewRequest("GET", "https://fixture.invalid/read", nil)
		r.TLS = &tls.ConnectionState{}
		r.Header.Set("Authorization", "Bearer 12345678-1234-4234-8234-123456789abc")
		fixture.change(r)
		err := cloudFileReadCB(httptest.NewRecorder(), r)
		if err == nil || err.Code != fixture.status {
			t.Errorf("request boundary returned %#v; want %d", err, fixture.status)
		}
	}
}

func TestCloudFileReadHeadersRequireCurrentAuthority(t *testing.T) {
	response := httptest.NewRecorder()
	tracked := &cloudFileTrackedResponse{ResponseWriter: response}
	w := &cloudFileReadWriter{context: context.Background(), check: func() error { return errCloudFileReadEnded }}
	guarded := &cloudFileGuardedResponse{ResponseWriter: tracked, writer: w}
	guarded.WriteHeader(http.StatusPartialContent)
	if tracked.committed || w.failed != errCloudFileReadEnded {
		t.Fatal("invalidated headers released")
	}
	if n, err := guarded.Write([]byte("private")); n != 0 || err != errCloudFileReadEnded {
		t.Fatal("invalidated body released")
	}
}

func TestTrackedReadCountsOnlyAcceptedBytes(t *testing.T) {
	tracked := &cloudFileTrackedResponse{ResponseWriter: httptest.NewRecorder()}
	if n, err := tracked.Write([]byte("abc")); n != 3 || err != nil || tracked.bytes != 3 || !tracked.committed {
		t.Fatal("actual response byte count lost")
	}
}

func TestTrackedReadFinalStatusCannotBeOverwritten(t *testing.T) {
	tracked := &cloudFileTrackedResponse{ResponseWriter: httptest.NewRecorder()}
	tracked.WriteHeader(http.StatusEarlyHints)
	if tracked.committed || tracked.status != 0 {
		t.Fatal("informational response committed a transfer")
	}
	tracked.WriteHeader(http.StatusPartialContent)
	tracked.WriteHeader(http.StatusInternalServerError)
	if tracked.status != http.StatusPartialContent {
		t.Fatal("later status overwrote the committed response")
	}
}

func TestReadOutcomeDoesNotClaimClientReceipt(t *testing.T) {
	for _, fixture := range []struct {
		method           string
		status           int
		bytes, expected  uint64
		known, cancelled bool
		failure          error
		result           string
	}{
		{http.MethodGet, 200, 3, 3, true, false, nil, "stream_completed"},
		{http.MethodGet, 206, 2, 2, true, false, nil, "stream_completed"},
		{http.MethodGet, 200, 0, 0, true, false, nil, "stream_completed"},
		{http.MethodGet, 200, 2, 3, true, false, nil, "interrupted"},
		{http.MethodGet, 200, 3, 3, false, false, nil, "interrupted"},
		{http.MethodGet, 200, 3, 3, true, true, nil, "interrupted"},
		{http.MethodGet, 200, 3, 3, true, false, errCloudFileReadEnded, "interrupted"},
		{http.MethodGet, 404, 3, 3, true, false, nil, "failed"},
		{http.MethodGet, 0, 0, 0, false, false, errCloudFileReadEnded, "failed"},
		{http.MethodHead, 200, 0, 3, true, false, nil, "succeeded"},
	} {
		tracked := &cloudFileTrackedResponse{status: fixture.status, committed: fixture.status != 0, bytes: fixture.bytes}
		value := tracked.outcome(fixture.method, fixture.expected, fixture.known, fixture.failure, fixture.cancelled)
		if value.Result != fixture.result || value.Status != fixture.status || value.BytesSent != fixture.bytes {
			t.Fatalf("unexpected terminal observation: %#v for %#v", value, fixture)
		}
	}
}

func TestManagedReadDoesNotInheritLegacyCORS(t *testing.T) {
	for _, explicitHeader := range []bool{false, true} {
		response := httptest.NewRecorder()
		tracked := &cloudFileTrackedResponse{ResponseWriter: response}
		tracked.Header().Set("Access-Control-Allow-Origin", "*")
		tracked.Header().Set("Access-Control-Allow-Credentials", "true")
		tracked.Header().Set("Access-Control-Expose-Headers", "Content-Disposition")
		tracked.Header().Set("Cache-Control", "public, max-age=3600")
		if explicitHeader {
			tracked.WriteHeader(http.StatusPartialContent)
		} else {
			_, _ = tracked.Write([]byte("file"))
		}
		result := response.Result()
		for _, name := range []string{"Access-Control-Allow-Origin", "Access-Control-Allow-Credentials", "Access-Control-Expose-Headers"} {
			if result.Header.Get(name) != "" {
				t.Fatalf("legacy %s escaped into managed transfer", name)
			}
		}
		if result.Header.Get("Cache-Control") != "no-store, max-age=0" ||
			result.Header.Get("X-Content-Type-Options") != "nosniff" ||
			result.Header.Get("Referrer-Policy") != "no-referrer" {
			t.Fatal("managed transfer security headers missing")
		}
		_ = result.Body.Close()
	}
}
