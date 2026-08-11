// CloudFile extension seam in the Go fileserver.
//
// The desktop sync client is the one entry point that reaches file data
// without going through Seahub or the check_permission_by_path RPC: it
// authenticates with a repo token and then trades commits, fs objects and
// blocks over sync_api.go. Those carry no paths, so there is no per-file
// authorization point once a sync is under way.
//
// The only safe moment to say no is therefore before the sync starts, and the
// question to ask is "does this library contain anything this user may not
// read". Rather than reimplement any capability's rules a second time in Go,
// this asks seaf-server over RPC, so the C side stays the single authority.
//
// With no capability registered the RPC reports nothing restricted, so this
// is a no-op on a baseline build.
//
// Seahub asks the same question via is_repo_syncable so it can show a useful
// error. This check exists because a modified client can simply skip that.

package main

import (
	"encoding/json"
	"fmt"
	"sync"
	"time"

	"github.com/haiwen/seafile-server/fileserver/option"
)

const cfRestrictedExpireTime = 300

type cfRestrictedInfo struct {
	// Empty when the whole library is reachable.
	restrictedPath string
	revision       int64
	expireTime     int64
}

// cfAuthorityState is the explicit wire contract between the Go content
// boundary and C's ACL authority.  Only an explicitly disabled feature may
// pass through; every active failure or malformed reply becomes a denial.
type cfAuthorityState struct {
	State          string  `json:"state"`
	Revision       int64   `json:"revision"`
	RestrictedPath *string `json:"restricted_path"`
}

func cfReadAuthorityState(reply interface{}) (cfAuthorityState, bool) {
	raw, ok := reply.(string)
	if !ok {
		return cfAuthorityState{}, false
	}

	var authority cfAuthorityState
	if err := json.Unmarshal([]byte(raw), &authority); err != nil ||
		authority.State != "active-valid" || authority.Revision < 1 {
		return cfAuthorityState{}, false
	}
	return authority, true
}

var cfRestrictedCache sync.Map

var cfCallAuthority = func(repoID, user string) (interface{}, error) {
	return rpcclient.Call("cf_acl_authority_state", repoID, "/", user)
}

// cfFindRestrictedPath asks seaf-server for the first path in repoID that user
// cannot access at all. It returns "" when the library is fully reachable.
//
// A server without the RPC is a stock CE deployment only while the local
// switch is off.  Once the switch is on, a missing/malformed RPC reply is an
// active authority failure and must refuse sync.
func cfFindRestrictedPath(repoID, user string) string {
	if !option.CloudFileDirACLEnabled {
		return ""
	}

	key := fmt.Sprintf("%s:%s", repoID, user)
	now := time.Now().Unix()

	ret, err := cfCallAuthority(repoID, user)
	if err != nil {
		return "/"
	}

	authority, ok := cfReadAuthorityState(ret)
	if !ok {
		return "/"
	}

	if value, ok := cfRestrictedCache.Load(key); ok {
		info := value.(*cfRestrictedInfo)
		if info.expireTime > now && info.revision == authority.Revision {
			return info.restrictedPath
		}
		cfRestrictedCache.Delete(key)
	}

	restricted := ""
	if authority.RestrictedPath != nil {
		restricted = *authority.RestrictedPath
	}

	cfRestrictedCache.Store(key, &cfRestrictedInfo{
		restrictedPath: restricted,
		revision:       authority.Revision,
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
