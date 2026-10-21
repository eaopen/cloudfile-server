package main

import (
	"context"
	"net/http"

	"github.com/haiwen/seafile-server/fileserver/option"
)

// Seahub validates the public-link token. Directory policy only affects a
// public link if it intersects the target or its shared directory subtree.
func cfACLPublicLinkScope(repoID, target string, subtree bool) *appError {
	if len(repoID) != 36 || !cfACLCanonical(target) || seafileDB == nil {
		return &appError{errCFACL, "Public link scope unavailable", http.StatusServiceUnavailable}
	}
	ctx, cancel := context.WithTimeout(context.Background(), option.DBOpTimeout)
	defer cancel()
	var restricted bool
	err := seafileDB.QueryRowContext(ctx, `SELECT EXISTS(SELECT 1 FROM cf_dir_acl
        WHERE repo_id=? AND (path='/' OR path=? OR ? LIKE CONCAT(path,'/%')
        OR (? AND ((?='/' AND path<>'/') OR path LIKE CONCAT(?,'/%')))))`,
		repoID, target, target, subtree, target, target).Scan(&restricted)
	if err != nil {
		return &appError{errCFACL, "Public link ACL unavailable", http.StatusServiceUnavailable}
	}
	if restricted {
		return &appError{errCFACL, "Public link intersects directory policy", http.StatusForbidden}
	}
	return nil
}
