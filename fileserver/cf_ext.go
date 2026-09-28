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

import "github.com/haiwen/seafile-server/fileserver/option"

var cfCallRestrictedPath = func(repoID, user string) (interface{}, error) {
	return rpcclient.Call("cf_find_restricted_path", repoID, "/", user)
}

// cfFindRestrictedPath asks seaf-server for the first path in repoID that user
// cannot access at all. It returns "" when the library is fully reachable.
//
// The MVP deliberately does not cache this decision. A rule change therefore
// takes effect on the next sync request without a revision table or a second
// cache-invalidation protocol. Once enabled, an unavailable or malformed
// authority reply refuses sync at the repository root.
func cfFindRestrictedPath(repoID, user string) string {
	if !option.CloudFileDirACLEnabled {
		return ""
	}

	ret, err := cfCallRestrictedPath(repoID, user)
	if err != nil {
		return "/"
	}
	if ret == nil {
		return ""
	}
	path, ok := ret.(string)
	if !ok {
		return "/"
	}
	return path
}
