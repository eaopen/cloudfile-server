package main

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/golang-jwt/jwt/v5"
	"github.com/google/uuid"
	"github.com/haiwen/seafile-server/fileserver/option"
)

func TestCFEditWithoutGuardStillChecksUserAuthorization(t *testing.T) {
	prior := option.V2GuardAvailable
	option.V2GuardAvailable = false
	defer func() { option.V2GuardAvailable = prior }()
	req := httptest.NewRequest("POST", "/cf-edit/v1/checkout", nil)
	rec := httptest.NewRecorder()
	cfEditAPI(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("ordinary edit without Guard must still authenticate: HTTP %d", rec.Code)
	}
}
func TestCFEditScopedTicketValidation(t *testing.T) {
	prev := option.JWTPrivateKey
	option.JWTPrivateKey = "unit-test-secret"
	defer func() { option.JWTPrivateKey = prev }()
	now := time.Now()
	user := uuid.New().String()
	sign := func(mode, path string, expiry time.Duration) string {
		c := cfEditClaims{UID: "uid-test", RepoID: user, FilePath: path, Mode: mode, BaseID: "a" + "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
			RegisteredClaims: jwt.RegisteredClaims{Issuer: "cloudfile-v2-edit",
				Audience: jwt.ClaimStrings{"cloudfile-edit-agent"}, Subject: "user@auth.local",
				IssuedAt: jwt.NewNumericDate(now), ExpiresAt: jwt.NewNumericDate(now.Add(expiry))}}
		v, _ := jwt.NewWithClaims(jwt.SigningMethodHS256, c).SignedString([]byte(option.JWTPrivateKey))
		return v
	}
	for _, tc := range []struct {
		mode, path string
		expect     bool
	}{
		{"local-view", "/design.step", true},
		{"optimistic-edit", "/design.step", true},
		{"exclusive-edit", "/design.step", true},
		{"ordinary", "/design.step", false},
		{"optimistic-edit", "/a/../b", false},
	} {
		req := httptest.NewRequest("GET", "/cf-edit/v1/read", nil)
		req.Header.Set("Authorization", "Bearer "+sign(tc.mode, tc.path, time.Minute))
		_, err := cfEditClaimsFor(req)
		if (err == nil) != tc.expect {
			t.Fatalf("mode %s path %s: %v", tc.mode, tc.path, err)
		}
	}
	req := httptest.NewRequest("GET", "/cf-edit/v1/read", nil)
	req.Header.Set("Authorization", "Bearer "+sign("exclusive-edit", "/design.step", -time.Minute))
	if _, err := cfEditClaimsFor(req); err == nil {
		t.Fatal("expired ticket accepted")
	}
}
func TestCFEditCredentialDigestDoesNotTrustUnrelatedActor(t *testing.T) {
	c := &cfEditClaims{RegisteredClaims: jwt.RegisteredClaims{Subject: "user@auth.local"}}
	p := cfEditProof{GuardID: uuid.New().String(), Generation: 3, Proof: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}
	g := cfEditGuard{ID: p.GuardID, Generation: 3, Owner: "user@auth.local",
		ProofDigest: fmt.Sprintf("%x", cfEditHash(p.Proof)), Expires: time.Now().Add(time.Minute)}
	if !cfEditVerified(c, g, p, false) {
		t.Fatal("matching guard rejected")
	}
	p.Proof = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
	if cfEditVerified(c, g, p, false) {
		t.Fatal("invalid guard proof accepted")
	}
}

func TestCFEditPeerIPOnlyTrustsLoopbackProxy(t *testing.T) {
	cases := []struct{ remote, header, expected string }{
		{"127.0.0.1:4321", "203.0.113.21", "203.0.113.21"},
		{"[::1]:4321", "2001:db8::21", "2001:db8::21"},
		{"198.51.100.8:4321", "203.0.113.21", "198.51.100.8"},
		{"127.0.0.1:4321", "not-an-ip", "127.0.0.1"},
	}
	for _, tc := range cases {
		r := httptest.NewRequest("GET", "/cf-edit/v1/status", nil)
		r.RemoteAddr = tc.remote
		r.Header.Set("X-Real-IP", tc.header)
		if got := cfEditPeerIP(r); got != tc.expected {
			t.Fatalf("remote=%s header=%s got=%s want=%s", tc.remote, tc.header, got, tc.expected)
		}
	}
}

func TestCFEditLapsedGuardResumesOnlyOnUnchangedFile(t *testing.T) {
	now := time.Now().UTC()
	base := strings.Repeat("a", 40)
	live := cfEditGuard{BaseID: base, Expires: now.Add(time.Minute)}
	lapsed := cfEditGuard{BaseID: base, Expires: now.Add(-time.Minute)}
	if !cfEditResumable(live, "", now) {
		t.Fatal("live lease must renew without a file lookup")
	}
	if !cfEditResumable(lapsed, base, now) {
		t.Fatal("lapsed Guard on the unchanged file must resume")
	}
	if cfEditResumable(lapsed, strings.Repeat("b", 40), now) {
		t.Fatal("lapsed Guard resumed after another save changed the file")
	}
	if cfEditResumable(cfEditGuard{Expires: now.Add(-time.Minute)}, "", now) {
		t.Fatal("lapsed Guard without a base resumed")
	}
}

func TestCFEditExclusiveUnavailableWithoutCheckoutSchema(t *testing.T) {
	oldFlag, oldSecret := option.V2GuardAvailable, option.JWTPrivateKey
	option.V2GuardAvailable = false
	option.JWTPrivateKey = "unit-test-secret"
	defer func() { option.V2GuardAvailable = oldFlag; option.JWTPrivateKey = oldSecret }()
	now := time.Now()
	ticket := cfEditClaims{
		UID:      "uid-test",
		RepoID:   uuid.New().String(),
		FilePath: "/design.step",
		BaseID:   "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
		Mode:     "exclusive-edit",
		RegisteredClaims: jwt.RegisteredClaims{
			Issuer:    "cloudfile-v2-edit",
			Audience:  jwt.ClaimStrings{"cloudfile-edit-agent"},
			Subject:   "user@auth.local",
			IssuedAt:  jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(time.Minute)),
		},
	}
	signed, err := jwt.NewWithClaims(jwt.SigningMethodHS256, ticket).SignedString([]byte(option.JWTPrivateKey))
	if err != nil {
		t.Fatal(err)
	}
	req := httptest.NewRequest("POST", "/cf-edit/v1/checkout", nil)
	req.Header.Set("Authorization", "Bearer "+signed)
	rec := httptest.NewRecorder()
	cfEditAPI(rec, req)
	if rec.Code != http.StatusConflict || !strings.Contains(rec.Body.String(), "CHECKOUT_UNSUPPORTED") {
		t.Fatalf("unexpected disabled checkout response: %d %s", rec.Code, rec.Body.String())
	}
}

func TestCFEditOrdinaryRequiresNoCheckoutTable(t *testing.T) {
	cases := []struct {
		mode, route, action string
		allowed             bool
	}{
		{"local-view", "read", "", true},
		{"local-view", "status", "", false},
		{"local-view", "checkout", "", false},
		{"local-view", "publish", "optimistic", false},
		{"local-view", "publish", "commit", false},
		{"local-view", "renew", "", false},
		{"local-view", "release", "", false},
		{"optimistic-edit", "read", "", true},
		{"optimistic-edit", "status", "", true},
		{"optimistic-edit", "publish", "optimistic", true},
		{"optimistic-edit", "publish", "checkin", false},
		{"optimistic-edit", "checkout", "", false},
		{"optimistic-edit", "renew", "", false},
		{"exclusive-edit", "read", "", false},
		{"exclusive-edit", "publish", "commit", false},
		{"exclusive-edit", "checkout", "", false},
	}
	for _, c := range cases {
		if got := cfEditAllowedWithoutCheckout(c.mode, c.route, c.action); got != c.allowed {
			t.Fatalf("mode=%s route=%s action=%s allowed=%v, wanted %v",
				c.mode, c.route, c.action, got, c.allowed)
		}
	}
}

func TestCFEditReadOnlyNeverReachesWriteRoutesEvenWithGuard(t *testing.T) {
	for _, route := range []string{"status", "checkout", "renew", "release", "publish", "unknown"} {
		if cfEditReadOnlyRoute("local-view", route) {
			t.Fatalf("view-only grant was accepted on %s", route)
		}
	}
	if !cfEditReadOnlyRoute("local-view", "read") {
		t.Fatal("read-only ticket unexpectedly rejected for read")
	}
	if !cfEditReadOnlyRoute("optimistic-edit", "publish") ||
		!cfEditReadOnlyRoute("exclusive-edit", "checkout") {
		t.Fatal("existing edit routes incorrectly disabled")
	}
}

func TestCFEditReadOnlyDeniedBeforeAnyRepoOrCheckoutAction(t *testing.T) {
	oldKey, oldGuard := option.JWTPrivateKey, option.V2GuardAvailable
	defer func() {
		option.JWTPrivateKey = oldKey
		option.V2GuardAvailable = oldGuard
	}()
	option.JWTPrivateKey = "local-view-readonly-unit-test-key"
	option.V2GuardAvailable = true
	now := time.Now()
	c := cfEditClaims{
		UID: "uid-test",
		RepoID: uuid.New().String(),
		FilePath: "/design.step",
		Mode: "local-view",
		BaseID: strings.Repeat("a", 40),
		RegisteredClaims: jwt.RegisteredClaims{
			Issuer: "cloudfile-v2-edit",
			Audience: jwt.ClaimStrings{"cloudfile-edit-agent"},
			Subject: "reader@auth.local",
			IssuedAt: jwt.NewNumericDate(now),
			ExpiresAt: jwt.NewNumericDate(now.Add(time.Minute)),
		},
	}
	token, err := jwt.NewWithClaims(jwt.SigningMethodHS256, c).
		SignedString([]byte(option.JWTPrivateKey))
	if err != nil {
		t.Fatal(err)
	}
	for _, route := range []string{"checkout", "publish", "release", "renew", "status"} {
		req := httptest.NewRequest(http.MethodPost, "/cf-edit/v1/"+route, nil)
		req.Header.Set("Authorization", "Bearer "+token)
		rec := httptest.NewRecorder()
		cfEditAPI(rec, req)
		if rec.Code != http.StatusForbidden || !strings.Contains(rec.Body.String(), "READ_ONLY_TICKET") {
			t.Fatalf("local-view %s responded %d %s", route, rec.Code, rec.Body.String())
		}
	}
}
