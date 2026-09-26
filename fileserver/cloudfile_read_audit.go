package main

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"io"
	"math"
	"strconv"
	"strings"
	"time"
	"unicode"
	"unicode/utf8"

	"github.com/google/uuid"
)

// Private server-derived fact. The native adapter populates these fields from
// the consumed transfer, never from HTTP identity/path headers.
type cloudFileReadAuditFact struct {
	RequestID, UserID, RepoID, Path, HeadID, Epoch, Operation string
	Outcome                                                   cloudFileReadOutcome
}

func captureCloudFileReadAudit(token, requestID string) (cloudFileReadAuditFact, error) {
	if rpcclient == nil || !canonicalTicketUUID(token) || !canonicalTicketUUID(requestID) {
		return cloudFileReadAuditFact{}, errors.New("read audit metadata unavailable")
	}
	value, err := rpcclient.CallWithTimeout(5*time.Second, "seafile_cloudfile_read_transfer_fact", token)
	encoded, ok := value.(string)
	if err != nil || !ok {
		return cloudFileReadAuditFact{}, errors.New("read audit metadata unavailable")
	}
	return decodeCloudFileReadAudit(encoded, requestID)
}

func decodeCloudFileReadAudit(encoded, requestID string) (cloudFileReadAuditFact, error) {
	failure := errors.New("read audit metadata unavailable")
	if len(encoded) > 16384 {
		return cloudFileReadAuditFact{}, failure
	}
	decoder := json.NewDecoder(strings.NewReader(encoded))
	opening, err := decoder.Token()
	if err != nil || opening != json.Delim('{') {
		return cloudFileReadAuditFact{}, failure
	}
	fields := make(map[string]string)
	for decoder.More() {
		key, err := decoder.Token()
		name, ok := key.(string)
		if err != nil || !ok {
			return cloudFileReadAuditFact{}, failure
		}
		if _, duplicate := fields[name]; duplicate {
			return cloudFileReadAuditFact{}, failure
		}
		var value string
		if decoder.Decode(&value) != nil {
			return cloudFileReadAuditFact{}, failure
		}
		fields[name] = value
	}
	closing, err := decoder.Token()
	if err != nil || closing != json.Delim('}') || len(fields) != 6 {
		return cloudFileReadAuditFact{}, failure
	}
	if _, err = decoder.Token(); err != io.EOF {
		return cloudFileReadAuditFact{}, failure
	}
	fact := cloudFileReadAuditFact{RequestID: requestID, UserID: fields["user_id"], RepoID: fields["repo_id"],
		Path: fields["path"], HeadID: fields["head_id"], Epoch: fields["epoch"], Operation: fields["operation"],
		Outcome: cloudFileReadOutcome{Result: "failed"}}
	if !fact.valid() {
		return cloudFileReadAuditFact{}, failure
	}
	return fact, nil
}

func (f cloudFileReadAuditFact) valid() bool {
	if !canonicalTicketUUID(f.RequestID) || !canonicalTicketUUID(f.RepoID) ||
		f.UserID == "" || len([]rune(f.UserID)) > 225 || !utf8.ValidString(f.UserID) ||
		strings.IndexFunc(f.UserID, unicode.IsControl) >= 0 ||
		!utf8.ValidString(f.Path) || len(f.Path) > 4096 || !strings.HasPrefix(f.Path, "/") ||
		strings.HasSuffix(f.Path, "/") || strings.Contains(f.Path, "//") || strings.ContainsRune(f.Path, 0) ||
		!lowerHex(f.HeadID, 40) || !lowerHex(f.Epoch, 32) ||
		(f.Operation != "view" && f.Operation != "download") || f.Outcome.BytesSent > math.MaxInt64 {
		return false
	}
	for _, segment := range strings.Split(f.Path, "/") {
		if segment == "." || segment == ".." {
			return false
		}
	}
	switch f.Outcome.Result {
	case "attempted", "succeeded", "stream_completed", "failed", "interrupted":
	default:
		return false
	}
	return f.Outcome.Status == 0 || (f.Outcome.Status >= 200 && f.Outcome.Status <= 599)
}

func lowerHex(value string, length int) bool {
	if len(value) != length {
		return false
	}
	for _, character := range value {
		if !((character >= '0' && character <= '9') || (character >= 'a' && character <= 'f')) {
			return false
		}
	}
	return true
}

// Atomic outbox+audit append on the existing native database. Explicitly not
// called by the unregistered managed handler; release gates remain outstanding.
// A failed/uncertain commit is not retried under a fresh event identity.
func appendCloudFileReadAudit(ctx context.Context, database *sql.DB, fact cloudFileReadAuditFact) error {
	if ctx == nil || database == nil || !fact.valid() {
		return errors.New("invalid read audit runtime or fact")
	}
	deadline, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	transaction, err := database.BeginTx(deadline, nil)
	if err != nil {
		return err
	}
	defer transaction.Rollback()
	// A nontransactional table could preserve only half of the fact. Missing or
	// wrongly configured tables deny the transfer rather than degrading logging.
	var engines int
	if err = transaction.QueryRowContext(deadline, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE() AND table_name IN ('cf_event_outbox','cf_audit_event') AND engine='InnoDB'").Scan(&engines); err != nil || engines != 2 {
		return errors.New("transactional read audit schema unavailable")
	}
	now := time.Now().UTC()
	eventID := uuid.New().String()
	stream := "repo." + fact.RepoID
	result, err := transaction.ExecContext(deadline, "INSERT INTO cf_event_outbox(event_id,stream,schema_version,payload,created_at,audit_state,resource_state,resource_next_at,search_state,search_next_at) VALUES(?,?,1,'{}',UTC_TIMESTAMP(6),'done','queued',UTC_TIMESTAMP(6),'queued',UTC_TIMESTAMP(6))", eventID, stream)
	if err != nil {
		return err
	}
	sequence, err := result.LastInsertId()
	if err != nil || sequence < 1 {
		return errors.New("read audit sequence unavailable")
	}
	payload, err := json.Marshal(map[string]interface{}{
		"event_id": eventID, "occurred_at": now.Format(time.RFC3339Nano), "recorded_at": now.Format(time.RFC3339Nano),
		"request_id": fact.RequestID, "actor_user_id": fact.UserID, "actor_kind": "user", "source": "fileserver",
		"action": "file." + fact.Operation, "result": fact.Outcome.Result, "repo_id": fact.RepoID, "path": fact.Path,
		"resource_kind": "file", "content_version": fact.HeadID, "subject_revision": fact.Epoch,
		"bytes_sent": fact.Outcome.BytesSent, "schema_version": 1, "stream": stream, "sequence": strconv.FormatInt(sequence, 10),
	})
	if err != nil || len(payload) > 65536 {
		return errors.New("read audit payload unavailable")
	}
	if _, err = transaction.ExecContext(deadline, "UPDATE cf_event_outbox SET payload=? WHERE event_id=?", string(payload), eventID); err != nil {
		return err
	}
	_, err = transaction.ExecContext(deadline, "INSERT INTO cf_audit_event(repo_id,object_type,object_id,operation,operator,source,result,occurred_at,source_path,event_id,schema_version,recorded_at,request_id,actor_user_id,actor_kind,event_payload) VALUES(?,'file','',?,?,'fileserver',?,?,?, ?,1,?,?,?,'user',?)",
		fact.RepoID, "file."+fact.Operation, fact.UserID, fact.Outcome.Result, now, fact.Path, eventID, now, fact.RequestID, fact.UserID, string(payload))
	if err != nil {
		return err
	}
	return transaction.Commit()
}
