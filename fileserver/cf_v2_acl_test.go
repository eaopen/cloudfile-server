package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/haiwen/seafile-server/fileserver/diff"
	"github.com/haiwen/seafile-server/fileserver/option"
)

func TestCFACLCanonical(t *testing.T) {
	for _, value := range []string{"", "x", "/x/", "/x//y", "/x/../y", "/x\\y", "/x\x00", string([]byte{'/', 255})} {
		if cfACLCanonical(value) {
			t.Fatalf("accepted %q", value)
		}
	}
	for _, value := range []string{"/", "/共享/文件", "/a.txt"} {
		if !cfACLCanonical(value) {
			t.Fatalf("rejected %q", value)
		}
	}
}

func TestCFACLRejectsStructuralAndUnprovedChanges(t *testing.T) {
	for _, status := range []rune{diff.DiffStatusDeleted, diff.DiffStatusRenamed, diff.DiffStatusDirAdded, diff.DiffStatusDirDeleted, diff.DiffStatusDirRenamed} {
		if _, err := cfACLChanges([]*diff.DiffEntry{{Name: "shared", NewName: "other", Status: status}}); err == nil {
			t.Fatalf("accepted status %c", status)
		}
	}
	checks, err := cfACLChanges([]*diff.DiffEntry{{Name: "shared/file", Status: diff.DiffStatusModified}})
	if err != nil || len(checks) != 1 || checks[0].Path != "/shared/file" {
		t.Fatalf("bad checks %v %v", checks, err)
	}
}

func TestCFACLNativeProtocolRechecksAndFailsClosed(t *testing.T) {
	oldURL, oldKey := option.SeahubURL, option.JWTPrivateKey
	defer func() { option.SeahubURL, option.JWTPrivateKey = oldURL, oldKey }()
	option.JWTPrivateKey = "test-only-jwt-key-not-a-real-deployment-secret"
	allowed := true
	calls := 0
	reply := `{"allowed":true,"contract":"cf-acl-v1"}`
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.URL.Path != "/cloudfile/acl/check/" || !strings.HasPrefix(r.Header.Get("Authorization"), "Token ") {
			t.Error("missing native auth")
		}
		var body struct {
			Username string       `json:"username"`
			Checks   []cfACLCheck `json:"checks"`
		}
		if json.NewDecoder(r.Body).Decode(&body) != nil || body.Username != "current-user" || len(body.Checks) != 1 {
			t.Error("actor/path not bound")
		}
		if !allowed {
			w.WriteHeader(403)
			return
		}
		w.Write([]byte(reply))
	}))
	defer srv.Close()
	option.SeahubURL = srv.URL
	checks := []cfACLCheck{{"/shared/file", "file", "write"}}
	if err := cfACLAuthorize(context.Background(), "repo", "current-user", checks); err != nil {
		t.Fatal(err)
	}
	allowed = false
	if err := cfACLAuthorize(context.Background(), "repo", "current-user", checks); err == nil {
		t.Fatal("cached grant survived revocation")
	}
	if calls != 2 {
		t.Fatal("permission decision was cached")
	}
	allowed = true
	for _, bad := range []string{`{}`, `{"allowed":true}`, `{"allowed":true,"contract":"wrong"}`, `not json`} {
		reply = bad
		if cfACLAuthorize(context.Background(), "repo", "current-user", checks) == nil {
			t.Fatal("accepted malformed authority response")
		}
	}
	if cfACLAuthorize(context.Background(), "repo", "", checks) == nil {
		t.Fatal("accepted missing actor")
	}
}

func TestCFACLDisabledNeedsNoDB(t *testing.T) {
	old := option.V2ACLEnabled
	defer func() { option.V2ACLEnabled = old }()
	option.V2ACLEnabled = false
	if protected, err := cfACLProtected(context.Background(), nil, "repo"); err != nil || protected {
		t.Fatal("disabled changed native behavior")
	}
}
