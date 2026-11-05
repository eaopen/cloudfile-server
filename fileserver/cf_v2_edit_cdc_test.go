package main

import (
	"bytes"
	"github.com/golang-jwt/jwt/v5"
	"github.com/haiwen/seafile-server/fileserver/fsmgr"
	"github.com/haiwen/seafile-server/fileserver/option"
	"io"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestCFEditCDCReceiptScope(t *testing.T) {
	old := option.JWTPrivateKey
	option.JWTPrivateKey = "cdc-test-key"
	defer func() { option.JWTPrivateKey = old }()
	c := cfEditClaims{UID: "u", RepoID: "repo", FilePath: "/one.prt", Mode: "optimistic-edit", RegisteredClaims: jwt.RegisteredClaims{Subject: "alice"}}
	b := cfCDCBlock{ID: strings.Repeat("a", 40), Size: 123}
	base := strings.Repeat("b", 40)
	b.Receipt = cfCDCReceipt(&c, base, "gc-1", b)
	if !cfCDCReceiptValid(&c, base, "gc-1", b) {
		t.Fatal("valid receipt rejected")
	}
	for _, change := range []func(*cfEditClaims){func(c *cfEditClaims) { c.Subject = "bob" }, func(c *cfEditClaims) { c.UID = "other" }, func(c *cfEditClaims) { c.RepoID = "other" }, func(c *cfEditClaims) { c.FilePath = "/other.prt" }, func(c *cfEditClaims) { c.Mode = "local-view" }} {
		other := c
		change(&other)
		if cfCDCReceiptValid(&other, base, "gc-1", b) {
			t.Fatal("cross-scope receipt accepted")
		}
	}
	if cfCDCReceiptValid(&c, base, "gc-2", b) || cfCDCReceiptValid(&c, strings.Repeat("c", 40), "gc-1", b) {
		t.Fatal("stale generation/base accepted")
	}
	b.Size++
	if cfCDCReceiptValid(&c, base, "gc-1", b) {
		t.Fatal("tampered size accepted")
	}
	b.Size--
	b.ID = strings.Repeat("d", 40)
	if cfCDCReceiptValid(&c, base, "gc-1", b) {
		t.Fatal("tampered hash accepted")
	}
}
func TestCFEditCDCManifestBoundsAndRepeatedBlocks(t *testing.T) {
	m := cfCDCManifest{Base: strings.Repeat("a", 40), Digest: strings.Repeat("b", 64), Size: 2, Blocks: []cfCDCBlock{{ID: strings.Repeat("c", 40), Size: 1}, {ID: strings.Repeat("c", 40), Size: 1}}}
	if e := cfCDCValidate(&m); e != nil {
		t.Fatal(e)
	}
	m.Size++
	if cfCDCValidate(&m) == nil {
		t.Fatal("length mismatch accepted")
	}
	m.Size--
	m.Blocks[0].Size = cfCDCMaxBlock + 1
	if cfCDCValidate(&m) == nil {
		t.Fatal("oversized block accepted")
	}
	m.Blocks = make([]cfCDCBlock, cfCDCMaxBlocks+1)
	if cfCDCValidate(&m) == nil {
		t.Fatal("unbounded manifest accepted")
	}
	for _, body := range []string{`{}`, `{"unknown":1}`, `{} {}`} {
		if _, e := cfCDCDecode(httptest.NewRequest("POST", "/", strings.NewReader(body))); e == nil {
			t.Fatal("bad manifest accepted")
		}
	}
}
func TestCFEditCDCBoundedWriter(t *testing.T) {
	var out bytes.Buffer
	b := &cfCDCBoundedWriter{w: &out, remaining: 3}
	if _, e := io.WriteString(b, "ab"); e != nil {
		t.Fatal(e)
	}
	if _, e := io.WriteString(b, "cd"); e == nil {
		t.Fatal("overflow accepted")
	}
	if out.String() != "ab" || b.remaining != 1 {
		t.Fatal("overflow leaked bytes")
	}
}
func TestCFEditCDCNativeFileIdentity(t *testing.T) {
	// Same deterministic native-CDC vector as Agent: order and repeats matter.
	ids := []string{"ba662a00dba363c3684a1e8c29d9738f9069b370", "189e5f9f183e0da3eafa5c4d6444b9af90be3d40", "a16440c92207495b04298efa8a731bc95b456718", "85631f0c03adad716532f398f5a3b0102e943f11", "5b3904b5d1f68ae66166e4aab06a5d48c26b0380"}
	f, e := fsmgr.NewSeafile(1, 33554432, ids)
	if e != nil || f.FileID != "72a1aa0bcea4f7e2260656760d57eadcedcc951d" {
		t.Fatalf("native identity: %v %v", f, e)
	}
}
func TestCFEditCDCRoutesRejectReadOnly(t *testing.T) {
	for _, route := range []string{"capabilities", "cdc-plan", "cdc-block", "cdc-publish"} {
		if cfEditReadOnlyRoute("local-view", route) {
			t.Fatal("read-only ticket can stage/publish")
		}
		if !cfEditAllowedWithoutCheckout("optimistic-edit", route, "optimistic") {
			t.Fatalf("ordinary edit unnecessarily needs Guard: %s", route)
		}
	}
	if cfEditAllowedWithoutCheckout("optimistic-edit", "cdc-publish", "checkin") {
		t.Fatal("exclusive operation without Guard")
	}
}

func TestCFEditStreamingReadFailureAndOrder(t *testing.T) {
	req := httptest.NewRequest("GET", "/", nil)
	var out bytes.Buffer
	read := func(store, id string, w io.Writer) error { _, e := io.WriteString(w, id); return e }
	if e := cfEditStreamBlocks(req, &out, "repo", []string{"ab", "cd", "ab"}, 6, read); e != nil || out.String() != "abcdab" {
		t.Fatalf("%s %v", out.String(), e)
	}
	out.Reset()
	if cfEditStreamBlocks(req, &out, "repo", []string{"ab"}, 3, read) != io.ErrUnexpectedEOF {
		t.Fatal("truncated transfer accepted")
	}
	out.Reset()
	if cfEditStreamBlocks(req, &out, "repo", []string{"abcd"}, 3, read) == nil || out.Len() != 0 {
		t.Fatal("oversized block leaked")
	}
	broken := func(store, id string, w io.Writer) error { return io.ErrClosedPipe }
	if cfEditStreamBlocks(req, io.Discard, "repo", []string{"a"}, 1, broken) == nil {
		t.Fatal("storage failure hidden")
	}
}

func TestCFEditCDCRouterRequiresTicket(t *testing.T) {
	router := newHTTPRouter()
	for _, route := range []string{"capabilities", "cdc-plan", "cdc-block", "cdc-publish"} {
		w := httptest.NewRecorder()
		router.ServeHTTP(w, httptest.NewRequest("POST", "/cf-edit/v1/"+route, nil))
		if w.Code != 401 {
			t.Fatalf("%s bypassed auth or was not registered: %d", route, w.Code)
		}
	}
}

func TestCFEditCDCPlanDoesNotProbeUnrelatedHashes(t *testing.T) {
	c := &cfEditClaims{RepoID: "repo", FilePath: "/file", Mode: "optimistic-edit"}
	m := &cfCDCManifest{Base: strings.Repeat("a", 40), Blocks: []cfCDCBlock{{ID: "authorized", Size: 2}, {ID: "unrelated", Size: 2}, {ID: "staged", Size: 2}}}
	m.Blocks[2].Receipt = cfCDCReceipt(c, m.Base, "epoch", m.Blocks[2])
	visited := map[string]bool{}
	stat := func(id string) (int64, error) { visited[id] = true; return 2, nil }
	missing := cfCDCMissing(c, m, map[string]bool{"authorized": true}, "epoch", stat)
	if len(missing) != 1 || missing[0] != 1 || visited["unrelated"] || !visited["staged"] {
		t.Fatalf("missing=%v visited=%v", missing, visited)
	}
	missing = cfCDCMissing(c, m, map[string]bool{"authorized": true}, "new-epoch", stat)
	if len(missing) != 2 {
		t.Fatal("old GC receipt was reused")
	}
}
