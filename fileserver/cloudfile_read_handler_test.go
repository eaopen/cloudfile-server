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
