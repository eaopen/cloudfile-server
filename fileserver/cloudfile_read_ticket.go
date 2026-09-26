package main

import (
	"net/http"
	"strings"
	"unicode"
	"unicode/utf8"

	"github.com/haiwen/seafile-server/fileserver/utils"
)

// Consumes only the new guarded RPC. This is not registered as an HTTP route:
// the returned object metadata does not establish a future per-block grant.
func consumeCloudFileReadTicket(token string) (*webaccessInfo, *appError) {
	if !canonicalTicketUUID(token) {
		return nil, &appError{nil, "Invalid read ticket", http.StatusBadRequest}
	}
	if rpcclient == nil {
		return nil, &appError{nil, "Read ticket service unavailable", http.StatusServiceUnavailable}
	}
	value, err := rpcclient.Call("seafile_cloudfile_consume_read_ticket", token)
	if err != nil {
		// Do not serialize/log RPC errors, the bearer token or native user data.
		return nil, &appError{nil, "Read ticket unavailable", http.StatusServiceUnavailable}
	}
	return decodeCloudFileReadTicket(value)
}

func canonicalTicketUUID(value string) bool {
	return len(value) == 36 && value == strings.ToLower(value) &&
		value[8] == '-' && value[13] == '-' && value[18] == '-' && value[23] == '-' &&
		utils.IsValidUUID(value)
}

func decodeCloudFileReadTicket(value interface{}) (*webaccessInfo, *appError) {
	if value == nil {
		return nil, &appError{nil, "Read ticket unavailable", http.StatusForbidden}
	}
	fields, ok := value.(map[string]interface{})
	if !ok || len(fields) > 16 {
		return nil, &appError{nil, "Read ticket response unavailable", http.StatusServiceUnavailable}
	}
	repo, repoOK := fields["repo-id"].(string)
	object, objectOK := fields["obj-id"].(string)
	op, opOK := fields["op"].(string)
	user, userOK := fields["username"].(string)
	if !repoOK || !canonicalTicketUUID(repo) || !objectOK || !utils.IsObjectIDValid(object) ||
		!opOK || (op != "view" && op != "download") || !userOK ||
		len(user) == 0 || len(user) > 255 || !utf8.ValidString(user) ||
		strings.IndexFunc(user, unicode.IsControl) >= 0 {
		return nil, &appError{nil, "Read ticket response unavailable", http.StatusServiceUnavailable}
	}
	return &webaccessInfo{repoID: repo, objID: object, op: op, user: user}, nil
}
