package main

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
)

func TestNativeReadAuditMetadataStrictShape(t *testing.T) {
	requestID := "11111111-1111-1111-1111-111111111111"
	fields := map[string]string{"user_id": "business-user", "repo_id": requestID, "path": "/literal%2Ffile",
		"head_id": strings.Repeat("a", 40), "epoch": strings.Repeat("b", 32), "operation": "download"}
	encoded, _ := json.Marshal(fields)
	fact, err := decodeCloudFileReadAudit(string(encoded), requestID)
	if err != nil || fact.Path != "/literal%2Ffile" || fact.UserID != "business-user" {
		t.Fatal("native audit fact unavailable or decoded twice")
	}
	for _, invalid := range []string{"null", "[]", string(encoded) + "{}", strings.Replace(string(encoded), "{", "{\"user_id\":\"other\",", 1),
		strings.Replace(string(encoded), "{", "{\"ticket\":\"secret\",", 1)} {
		if _, err := decodeCloudFileReadAudit(invalid, requestID); err == nil {
			t.Fatal("invalid native metadata accepted")
		}
	}
}

func TestCloudFileReadAuditFactBoundaries(t *testing.T) {
	valid := cloudFileReadAuditFact{
		RequestID: "11111111-1111-1111-1111-111111111111", RepoID: "22222222-2222-2222-2222-222222222222",
		UserID: "business-user", Path: "/图纸%2FUG.prt", HeadID: strings.Repeat("a", 40), Epoch: strings.Repeat("b", 32), Operation: "download",
		Outcome: cloudFileReadOutcome{Result: "stream_completed", Status: 200, BytesSent: 3},
	}
	if !valid.valid() {
		t.Fatal("valid server fact rejected")
	}
	for _, change := range []func(*cloudFileReadAuditFact){
		func(f *cloudFileReadAuditFact) { f.RequestID = "client-selected" },
		func(f *cloudFileReadAuditFact) { f.UserID = "" },
		func(f *cloudFileReadAuditFact) { f.UserID = "bad\nuser" },
		func(f *cloudFileReadAuditFact) { f.Path = "/a/../b" },
		func(f *cloudFileReadAuditFact) { f.Path = "/folder/" },
		func(f *cloudFileReadAuditFact) { f.Path = "/a//b" },
		func(f *cloudFileReadAuditFact) { f.Path = "/" + strings.Repeat("图", 1366) },
		func(f *cloudFileReadAuditFact) { f.HeadID = strings.Repeat("A", 40) },
		func(f *cloudFileReadAuditFact) { f.Epoch = "cached" },
		func(f *cloudFileReadAuditFact) { f.Operation = "upload" },
		func(f *cloudFileReadAuditFact) { f.Outcome.Result = "client_received" },
		func(f *cloudFileReadAuditFact) { f.Outcome.Status = 103 },
		func(f *cloudFileReadAuditFact) { f.Outcome.BytesSent = uint64(math.MaxInt64) + 1 },
	} {
		fact := valid
		change(&fact)
		if fact.valid() {
			t.Fatalf("invalid fact accepted: %#v", fact)
		}
	}
}
