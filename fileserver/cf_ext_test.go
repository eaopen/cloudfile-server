package main

import (
	"errors"
	"testing"
)

// Native CE sync without a path proof is gated by its own operation scope.
// These tests must not reintroduce "managed library" classification.
func withRestrictedPathStub(t *testing.T,
	stub func(string, string) (interface{}, error)) {
	t.Helper()
	originalCall := cfCallRestrictedPath
	cfCallRestrictedPath = stub
	t.Cleanup(func() { cfCallRestrictedPath = originalCall })
}

func TestCfAclAuthorityAllowPassesThrough(t *testing.T) {
	withRestrictedPathStub(t, func(string, string) (interface{}, error) {
		return nil, nil
	})

	if got := cfFindRestrictedPath("repo", "user"); got != "" {
		t.Fatalf("unrestricted authority reply must pass through, got %q", got)
	}
}

func TestCfAclAuthorityFailureFailsClosed(t *testing.T) {
	withRestrictedPathStub(t, func(string, string) (interface{}, error) {
		return nil, errors.New("authority unavailable")
	})

	if got := cfFindRestrictedPath("repo", "user"); got != "/" {
		t.Fatalf("authority failure must deny at root, got %q", got)
	}
}

func TestCfAclDecisionIsNotCached(t *testing.T) {
	replies := []interface{}{nil, "/secret"}
	withRestrictedPathStub(t, func(string, string) (interface{}, error) {
		result := replies[0]
		replies = replies[1:]
		return result, nil
	})

	if got := cfFindRestrictedPath("repo", "user"); got != "" {
		t.Fatalf("first authority decision should allow, got %q", got)
	}
	if got := cfFindRestrictedPath("repo", "user"); got != "/secret" {
		t.Fatalf("next request must observe revocation, got %q", got)
	}
}

func TestCfAclMalformedReplyFailsClosed(t *testing.T) {
	withRestrictedPathStub(t, func(string, string) (interface{}, error) {
		return 42, nil
	})

	if got := cfFindRestrictedPath("repo", "user"); got != "/" {
		t.Fatalf("malformed authority reply must deny at root, got %q", got)
	}
}
