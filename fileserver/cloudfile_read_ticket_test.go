package main

import (
	"strings"
	"testing"
)

func TestCloudFileTicketResponseBoundaries(t *testing.T) {
	valid := map[string]interface{}{
		"repo-id": "12345678-1234-4234-8234-123456789abc",
		"obj-id":  strings.Repeat("a", 40), "op": "download", "username": "fixture@invalid.test",
	}
	if result, err := decodeCloudFileReadTicket(valid); err != nil || result.user != valid["username"] {
		t.Fatal("valid fixed read metadata rejected")
	}
	for _, fixture := range []struct {
		key   string
		value interface{}
	}{
		{"repo-id", "invalid"}, {"obj-id", strings.Repeat("A", 40)},
		{"op", "upload"}, {"op", "download-link"}, {"username", ""},
		{"username", "fixture\x00"}, {"username", strings.Repeat("a", 256)},
		{"username", 123}, {"obj-id", nil},
	} {
		candidate := make(map[string]interface{})
		for key, value := range valid {
			candidate[key] = value
		}
		candidate[fixture.key] = fixture.value
		if result, err := decodeCloudFileReadTicket(candidate); result != nil || err == nil {
			t.Errorf("invalid %s accepted", fixture.key)
		}
	}
	for _, value := range []interface{}{nil, "wrong", []string{"wrong"}} {
		if result, err := decodeCloudFileReadTicket(value); result != nil || err == nil {
			t.Fatal("invalid metadata shape accepted")
		}
	}
}
