// CloudFile directory ACL, as seen by the Go fileserver.
//
// The desktop sync client is the one entry point that reaches file data
// without going through Seahub or the check_permission_by_path RPC: it
// authenticates with a repo token and then trades commits, fs objects and
// blocks over sync_api.go. Those carry no paths, so there is no per-file
// authorization point once a sync is under way.
//
// The only safe moment to say no is therefore before the sync starts, and the
// question to ask is "does this library contain anything this user may not
// read". Rather than reimplement the ACL a fourth time in Go, this asks
// seaf-server over RPC, so cf-acl.c stays the single authority.
//
// Seahub asks the same question via is_repo_syncable so it can show a useful
// error. This check exists because a modified client can simply skip that.

package main

import (
	"fmt"
	"sync"
	"time"
)

const cfRestrictedExpireTime = 300

type cfRestrictedInfo struct {
	// Empty when the whole library is reachable.
	restrictedPath string
	expireTime     int64
}

var cfRestrictedCache sync.Map

// cfFindRestrictedPath asks seaf-server for the first path in repoID that user
// cannot access at all. It returns "" when the library is fully reachable.
//
// A server without the RPC registered -- an upstream CE build, or CloudFile
// with the feature switched off -- makes the call fail, which is treated as
// "nothing restricted" so that sync keeps working exactly as it does on stock
// CE. Enforcement is only ever added by an ACL that actually exists.
func cfFindRestrictedPath(repoID, user string) string {
	key := fmt.Sprintf("%s:%s", repoID, user)
	now := time.Now().Unix()

	if value, ok := cfRestrictedCache.Load(key); ok {
		info := value.(*cfRestrictedInfo)
		if info.expireTime > now {
			return info.restrictedPath
		}
		cfRestrictedCache.Delete(key)
	}

	ret, err := rpcclient.Call("cf_find_restricted_path", repoID, "/", user)
	if err != nil {
		return ""
	}

	restricted := ""
	if ret != nil {
		if path, ok := ret.(string); ok {
			restricted = path
		}
	}

	cfRestrictedCache.Store(key, &cfRestrictedInfo{
		restrictedPath: restricted,
		expireTime:     now + cfRestrictedExpireTime,
	})

	return restricted
}

// cfClearRestrictedCache drops expired entries. Called from the same sweep
// that prunes permCache.
func cfClearRestrictedCache() {
	now := time.Now().Unix()
	cfRestrictedCache.Range(func(key, value interface{}) bool {
		if info, ok := value.(*cfRestrictedInfo); ok && info.expireTime <= now {
			cfRestrictedCache.Delete(key)
		}
		return true
	})
}
