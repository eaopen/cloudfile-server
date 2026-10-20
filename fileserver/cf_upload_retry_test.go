package main

// Regression guard for the retry contract: only a moved Branch HEAD may be
// retried. Without this test a refactor could silently turn a schema/Guard
// fault into ten upload attempts reported as "concurrent upload".

import (
	"errors"
	"fmt"
	"testing"
)

func TestCFUploadRetriesOnlyHeadConflict(t *testing.T) {
	if !cfBranchConflictRetryable(ErrConflict) {
		t.Fatal("direct HEAD conflict must remain retryable")
	}
	if !cfBranchConflictRetryable(fmt.Errorf("wrapped: %w", ErrConflict)) {
		t.Fatal("wrapped HEAD conflict must remain retryable")
	}
	for _, err := range []error{
		errCFEditGuard,
		errCFACL,
		errors.New("Error 1176 (42000): Key 'cf_edit_guard_active' doesn't exist in table"),
		errors.New("SQL unavailable"),
		nil,
	} {
		if cfBranchConflictRetryable(err) {
			t.Fatalf("deterministic failure must never retry: %v", err)
		}
	}
}
