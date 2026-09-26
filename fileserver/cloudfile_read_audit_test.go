package main

import (
	"math"
	"strings"
	"testing"
)

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
