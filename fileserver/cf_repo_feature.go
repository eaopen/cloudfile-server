package main

import (
	"context"
	"database/sql"
)

type cfRepoFeatures struct{ checkout, protection bool }

// Use the publishing transaction for a current locking read. An unavailable
// policy must abort the operation before its candidate Branch can be published.
func cfReadRepoFeatures(ctx context.Context, repo string, tx *sql.Tx) (features cfRepoFeatures, err error) {
	query := "SELECT feature FROM cf_repo_feature WHERE repo_id=? ORDER BY feature"
	var rows *sql.Rows
	if tx != nil {
		rows, err = tx.QueryContext(ctx, query+" LOCK IN SHARE MODE", repo)
	} else {
		if seafileDB == nil {
			return features, errCFProtectionUnavailable
		}
		rows, err = seafileDB.QueryContext(ctx, query, repo)
	}
	if err != nil {
		return features, err
	}
	defer rows.Close()
	for rows.Next() {
		var name string
		if err = rows.Scan(&name); err != nil {
			return features, err
		}
		switch name {
		case "protection":
			features.protection = true
		case "checkout":
			features.checkout = true
		default:
			return features, errCFProtectionUnavailable
		}
	}
	return features, rows.Err()
}
