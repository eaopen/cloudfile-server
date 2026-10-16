package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestCloudFileBrowserReadRejectsAmbiguousOrInvalidCredentials(t *testing.T) {
	for _, path := range []string{"/files/token/name", "/cloudfile/files/invalid/name", "/cloudfile/files/22222222-2222-4222-8222-222222222222/a/b", "/cloudfile/files/22222222-2222-4222-8222-222222222222/"} {
		r := httptest.NewRequest(http.MethodGet, "https://files.invalid"+path, nil)
		if err := cloudFileBrowserReadCB(httptest.NewRecorder(), r); err == nil || err.Code != http.StatusBadRequest {
			t.Fatalf("invalid browser path was not refused: %s", path)
		}
	}
	for _, header := range []string{"Authorization", "X-CloudFile-Filename"} {
		r := httptest.NewRequest(http.MethodGet, "https://files.invalid/cloudfile/files/22222222-2222-4222-8222-222222222222/name", nil)
		r.Header.Set(header, "client-selected")
		if err := cloudFileBrowserReadCB(httptest.NewRecorder(), r); err == nil || err.Code != http.StatusBadRequest {
			t.Fatalf("ambiguous %s was accepted", header)
		}
	}
}

func TestCloudFileBrowserReadKeepsReaderMethodAndTransportChecks(t *testing.T) {
	for _, tc := range []struct {
		method string
		url    string
		status int
	}{
		{http.MethodPost, "https://files.invalid", http.StatusMethodNotAllowed},
		{http.MethodGet, "http://files.invalid", http.StatusUnauthorized},
	} {
		r := httptest.NewRequest(tc.method, tc.url+"/cloudfile/files/22222222-2222-4222-8222-222222222222/name", nil)
		r.Header.Set("Cookie", "ambient=ignored")
		if err := cloudFileBrowserReadCB(httptest.NewRecorder(), r); err == nil || err.Code != tc.status {
			t.Fatal("browser adapter bypassed guarded reader checks")
		}
		if r.Header.Get("Cookie") != "ambient=ignored" || r.Header.Get("Authorization") != "" {
			t.Fatal("browser adapter mutated the original request")
		}
	}
}

func TestCloudFileBrowserReadRouteUsesGuardedAuthorityDespiteAmbientCookies(t *testing.T) {
	previous := rpcclient
	rpcclient = nil
	defer func() { rpcclient = previous }()
	r := httptest.NewRequest(http.MethodGet, "https://files.invalid/cloudfile/files/22222222-2222-4222-8222-222222222222/a%20%2B%25.pdf", nil)
	r.Header.Set("Cookie", "sessionid=unrelated")
	w := httptest.NewRecorder()
	newHTTPRouter().ServeHTTP(w, r)
	if w.Code != http.StatusServiceUnavailable || w.Header().Get("Cache-Control") != "no-store, max-age=0" {
		t.Fatal("browser read bypassed the missing authority or lost cache protection")
	}
	if r.Header.Get("Cookie") != "sessionid=unrelated" || r.Header.Get("Authorization") != "" {
		t.Fatal("browser read mutated ambient credentials")
	}
}
