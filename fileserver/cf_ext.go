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
//
// 2026-10-15, v0.9-only: the [cloudfile] dir_acl_enabled switch no longer
// gates this call. Once the deployment has the managed schema installed the
// legacy sync entry must be refused for managed libraries, and an off switch
// must not reopen them. The C side answers 0/"" for non-managed repositories,
// so plain CE keeps native behaviour.

package main

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
