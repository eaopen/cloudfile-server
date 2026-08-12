package main

import (
	"errors"
	"testing"

	"github.com/haiwen/seafile-server/fileserver/option"
)

func withRestrictedPathStub(t *testing.T, enabled bool,
	stub func(string, string) (interface{}, error)) {
	t.Helper()
	originalEnabled := option.CloudFileDirACLEnabled
	originalCall := cfCallRestrictedPath
	option.CloudFileDirACLEnabled = enabled
	cfCallRestrictedPath = stub
	t.Cleanup(func() {
		option.CloudFileDirACLEnabled = originalEnabled
		cfCallRestrictedPath = originalCall
	})
}

func TestCfAclDisabledPassesThrough(t *testing.T) {
	called := false
	withRestrictedPathStub(t, false, func(string, string) (interface{}, error) {
		called = true
		return nil, errors.New("must not be called")
	})

	if got := cfFindRestrictedPath("repo", "user"); got != "" || called {
		t.Fatalf("disabled ACL must pass through, got=%q called=%v", got, called)
	}
}

func TestCfAclEnabledFailsClosed(t *testing.T) {
	withRestrictedPathStub(t, true, func(string, string) (interface{}, error) {
		return nil, errors.New("authority unavailable")
	})

	if got := cfFindRestrictedPath("repo", "user"); got != "/" {
		t.Fatalf("authority failure must deny at root, got %q", got)
	}
}

func TestCfAclDecisionIsNotCached(t *testing.T) {
	replies := []interface{}{nil, "/secret"}
	withRestrictedPathStub(t, true, func(string, string) (interface{}, error) {
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
	withRestrictedPathStub(t, true, func(string, string) (interface{}, error) {
		return 42, nil
	})

	if got := cfFindRestrictedPath("repo", "user"); got != "/" {
		t.Fatalf("malformed authority reply must deny at root, got %q", got)
	}
}
