package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/haiwen/seafile-server/fileserver/option"
)

func TestCFWriterProbeRequiresDedicatedTicket(t *testing.T) {
	previous := option.JWTPrivateKey
	option.JWTPrivateKey = "isolated-writer-probe-test-key"
	t.Cleanup(func() { option.JWTPrivateKey = previous })
	router := newHTTPRouter()
	for _, audience := range []string{"", "cloudfile-edit-agent"} {
		r := httptest.NewRequest(http.MethodGet, "/internal/cf-writer-capabilities/v1", nil)
		if audience != "" {
			token, err := jwt.NewWithClaims(jwt.SigningMethodHS256, jwt.MapClaims{
				"iss": "cloudfile-hub", "aud": audience, "exp": time.Now().Add(time.Minute).Unix(),
			}).SignedString([]byte(option.JWTPrivateKey))
			if err != nil {
				t.Fatal(err)
			}
			r.Header.Set("Authorization", "Bearer "+token)
		}
		w := httptest.NewRecorder()
		router.ServeHTTP(w, r)
		if w.Code != http.StatusForbidden {
			t.Fatalf("audience %q: got %d", audience, w.Code)
		}
	}
}
