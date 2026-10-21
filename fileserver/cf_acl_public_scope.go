package main

import (
    "context"
    "net/http"

    "github.com/haiwen/seafile-server/fileserver/option"
)

// A public share-link is authorized independently by Seahub. A directory ACL
// only restricts that link if it intersects the path or its shared subtree.
// Unrelated ACL rows never classify or disable the repository.
func cfACLPublicLinkScope(repoID, target string, subtree bool) *appError {
    if len(repoID) != 36 || !cfACLCanonical(target) || seafileDB == nil {
        return &appError{errCFACL, "Public link scope unavailable", http.StatusServiceUnavailable}
    }
    ctx, cancel := context.WithTimeout(context.Background(), option.DBOpTimeout)
    defer cancel()
    var restricted bool
    err := seafileDB.QueryRowContext(ctx, `SELECT EXISTS(SELECT 1 FROM cf_dir_acl
        WHERE repo_id=? AND (path='/' OR path=? OR ? LIKE CONCAT(path,'/%')
        OR (? AND path LIKE CONCAT(?,'/%'))))`,
        repoID, target, target, subtree, target).Scan(&restricted)
    if err != nil {
        return &appError{errCFACL, "Public link ACL unavailable", http.StatusServiceUnavailable}
    }
    if restricted {
        return &appError{errCFACL, "Public link intersects directory policy", http.StatusForbidden}
    }
    return nil
}
