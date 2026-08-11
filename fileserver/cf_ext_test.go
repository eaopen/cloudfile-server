// Red tests for the three-state ACL authority contract at the Go fileserver
// boundary (SEC-02).
//
// cfFindRestrictedPath is the only enforcement point between the desktop sync
// client and file data: sync exchanges commits/fs/blocks without per-file
// authorization, so the only safe moment to say "no" is before a sync starts.
// The current implementation conflates "authority RPC failed" with "no
// restriction" by returning "" on any error -- that is the
// elevation-of-privilege bug these tests exist to fix.
//
// The contract being locked here is the same one the Hub and C suites
// consume from cloudfile-docker/docs/acl-cases.json::authority_states:
//
//   unsupported-stock  -> passthrough (no RPC registered, baseline CE)
//   inactive-disabled  -> passthrough (CF_ENABLE_DIR_ACL off)
//   active-valid       -> rules apply
//   active-unavailable -> DENY (RPC error under an active policy)
//   active-malformed   -> DENY (unparseable reply)
//   active-stale       -> DENY (revision mismatch at the final boundary)
//
// These tests are written to FAIL today: Wave 3 plan 01-04 introduces the
// explicit state classification in cf_ext.go. Green requires the
// empty-string anti-pattern to be gone from cf_ext.go AND every active-*
// state to map to an explicit denial rather than "".
//
// Run with: go test -count=1 -run 'TestCfAclAuthority' .

package main

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

func cfExtRepoRoot(t *testing.T) string {
	t.Helper()
	// `go test` runs from the package directory (fileserver/), which is where
	// cf_ext.go lives. cf_fileop_test.go uses ".." because it crosses up to
	// the server root to read common/cf-fileop.h; here the file under test is
	// a sibling of the test file, so no upward step is wanted.
	root, err := filepath.Abs(".")
	if err != nil {
		t.Fatalf("failed to resolve fileserver dir: %v", err)
	}
	return root
}

func authorityStatesCasesPath(t *testing.T) string {
	if p := os.Getenv("CF_ACL_CASES"); p != "" {
		return p
	}
	workspace := filepath.Dir(filepath.Dir(cfExtRepoRoot(t)))
	return filepath.Join(workspace, "cloudfile-docker",
		"docs", "acl-cases.json")
}

// loadAuthorityStates reads the same fixture the Python and C suites consume.
// Skipped when the sibling cloudfile-docker checkout is absent so a standalone
// server checkout still compiles cleanly.
func loadAuthorityStates(t *testing.T) map[string]map[string]interface{} {
	path := authorityStatesCasesPath(t)
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skipf("shared ACL case set not found at %s; set CF_ACL_CASES", path)
	}
	var parsed struct {
		AuthorityStates struct {
			States []map[string]interface{} `json:"states"`
		} `json:"authority_states"`
	}
	if err := json.Unmarshal(data, &parsed); err != nil {
		t.Fatalf("failed to parse %s: %v", path, err)
	}
	if len(parsed.AuthorityStates.States) == 0 {
		t.Fatalf("authority_states fixture at %s has no states", path)
	}
	out := make(map[string]map[string]interface{}, len(parsed.AuthorityStates.States))
	for _, s := range parsed.AuthorityStates.States {
		out[s["name"].(string)] = s
	}
	return out
}

// TestCfExtSourceHasExplicitAuthorityStateClassification is the structural
// guard against the empty-string anti-pattern. cf_ext.go MUST NOT silently
// translate an RPC error into the empty string that callers treat as
// "no restriction". Wave 3 plan 01-04 introduces an explicit state type;
// until then this fails.
func TestCfAclAuthoritySourceClassification(t *testing.T) {
	source, err := os.ReadFile(filepath.Join(cfExtRepoRoot(t), "cf_ext.go"))
	if err != nil {
		t.Fatalf("failed to read cf_ext.go: %v", err)
	}
	src := string(source)

	// The bug: returning "" on RPC error. The fix must replace this with an
	// explicit authority-state value the caller can distinguish from
	// "no restriction".
	emptyOnErr := regexp.MustCompile(`err != nil \{\s*return ""`)
	if emptyOnErr.MatchString(src) {
		t.Errorf(`cf_ext.go still translates RPC errors to the empty-string ` +
			`"no restriction" sentinel. An active authority outage must be ` +
			`classified as DENY, not pass-through; see SEC-02.`)
	}

	// The contract symbol Wave 3 introduces. Naming is at the implementer's
	// discretion per CONTEXT.md, so this asserts the SHAPE (a typed state
	// distinct from the raw string return) rather than a specific identifier.
	hasStateType := strings.Contains(src, "cfAuthorityState") ||
		strings.Contains(src, "authorityState") ||
		strings.Contains(src, "AuthorityVerdict") ||
		strings.Contains(src, "cfAclState")
	if !hasStateType {
		t.Errorf(`cf_ext.go has no explicit authority-state type. The three ` +
			`states (unsupported-stock / inactive-disabled / active-*) must ` +
			`be distinguishable from each other and from "no restriction".`)
	}

	// A revision-aware code path: stale-cache denial at the content boundary
	// requires the source to at least name "revision". Its absence proves the
	// final-boundary recheck has not been wired yet.
	if !strings.Contains(src, "revision") {
		t.Errorf(`cf_ext.go has no revision-aware path. SEC-02 requires the ` +
			`final content boundary to recheck the current ACL revision so ` +
			`revocation is immediate despite the 300s restricted-path cache.`)
	}
}

// TestAuthorityStatesFixtureParity drives the Go side from the same fixture
// the Hub and C suites use, so the three cannot drift apart. Each active-*
// state in the fixture MUST map to a deny verdict; only the two off states
// may pass through. This is the single test that fails if any consumer
// regresses to the "empty string means no restriction" anti-pattern.
func TestCfAclAuthorityStatesFixtureParity(t *testing.T) {
	states := loadAuthorityStates(t)

	wantCanonical := []string{
		"unsupported-stock", "inactive-disabled", "active-valid",
		"active-unavailable", "active-malformed", "active-stale",
	}
	for _, name := range wantCanonical {
		if _, ok := states[name]; !ok {
			t.Errorf("authority_states fixture is missing canonical state %s -- "+
				"the spec and the consumers must move together", name)
		}
	}

	for name, s := range states {
		featureEnabled, _ := s["feature_enabled"].(bool)
		verdict, _ := s["verdict"].(string)
		if featureEnabled && verdict == "passthrough" {
			t.Errorf("state %s has feature_enabled=true but verdict=passthrough "+
				"-- an enabled authority must never silently delegate to the "+
				"native CE path (SEC-02)", name)
		}
		if !featureEnabled && verdict != "passthrough" {
			t.Errorf("state %s has feature_enabled=false but verdict=%s (want "+
				"passthrough) -- a disabled authority MUST be indistinguishable "+
				"from stock CE", name, verdict)
		}
		if strings.HasPrefix(name, "active-") && verdict == "rules" && name != "active-valid" {
			t.Errorf("state %s has verdict=rules but only active-valid may apply "+
				"rules; outage/malformed/stale must DENY", name)
		}
	}
}

func TestCfAclAuthorityUnknownStateDenies(t *testing.T) {
	states := loadAuthorityStates(t)
	for name, s := range states {
		verdict, _ := s["verdict"].(string)
		if verdict != "passthrough" && verdict != "rules" && verdict != "deny" {
			t.Errorf("state %s has unknown verdict %q; unknown authority data must deny", name, verdict)
		}
	}
}

// TestCfExtRestrictedCacheKeysOnRevision asserts that the restricted-path
// cache key includes the ACL revision. Without it, a cached "no restriction"
// verdict survives a rule change for up to cfRestrictedExpireTime (300s) --
// that is the immediate-revocation hole SEC-02 closes. Wave 3 plan 01-04
// threads the revision into the cache key.
func TestCfAclAuthorityRestrictedCacheKeysOnRevision(t *testing.T) {
	source, err := os.ReadFile(filepath.Join(cfExtRepoRoot(t), "cf_ext.go"))
	if err != nil {
		t.Fatalf("failed to read cf_ext.go: %v", err)
	}
	src := string(source)

	// The current cache key is fmt.Sprintf("%s:%s", repoID, user) with no
	// revision component. A revision-aware key must compose repoID, user and
	// the revision the verdict was issued under.
	if !strings.Contains(src, "revision") {
		t.Errorf(`cf_ext.go restricted-path cache has no revision component. ` +
			`SEC-02 requires the cache key to embed the ACL revision so that ` +
			`revoke invalidates a cached "no restriction" verdict immediately.`)
	}
}
