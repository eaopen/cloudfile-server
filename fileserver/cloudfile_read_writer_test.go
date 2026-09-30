package main

import (
	"bytes"
	"context"
	"io"
	"testing"
)

func TestGuardedReadWriterChecksEachBlockAndStopsOnRevocation(t *testing.T) {
	var output bytes.Buffer
	calls := 0
	w := &cloudFileReadWriter{writer: &output, context: context.Background(), check: func() error {
		calls++
		if calls == 2 {
			return errCloudFileReadEnded
		}
		return nil
	}}
	n, err := w.Write(make([]byte, cloudFileReadBlock*3))
	if n != cloudFileReadBlock || err != errCloudFileReadEnded || output.Len() != cloudFileReadBlock {
		t.Fatal("revoked block was released or previous byte count lost")
	}
	if n, err := w.Write([]byte("more")); n != 0 || err != errCloudFileReadEnded || calls != 2 {
		t.Fatal("failed writer resumed")
	}
}

func TestGuardedReadWriterCancellationAndShortWrite(t *testing.T) {
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	calls := 0
	w := &cloudFileReadWriter{writer: io.Discard, context: ctx, check: func() error { calls++; return nil }}
	if n, err := w.Write([]byte("private")); n != 0 || err != context.Canceled || calls != 0 {
		t.Fatal("cancelled writer released content or called RPC")
	}
	w = &cloudFileReadWriter{writer: rangeFixtureWriter{n: 2}, context: context.Background(), check: func() error { return nil }}
	if n, err := w.Write([]byte("private")); n != 2 || err != io.ErrShortWrite {
		t.Fatal("short write treated as successful release")
	}
}

func TestGuardedReadWriterPreservesCompleteContent(t *testing.T) {
	var output bytes.Buffer
	calls := 0
	w := &cloudFileReadWriter{writer: &output, context: context.Background(), check: func() error { calls++; return nil }}
	content := bytes.Repeat([]byte("x"), cloudFileReadBlock+1)
	if n, err := w.Write(content); err != nil || n != len(content) || !bytes.Equal(output.Bytes(), content) || calls != 2 {
		t.Fatal("complete guarded content changed")
	}
}

func TestGuardedReadWriterCloseIsSingleAttemptAndBlocksWrites(t *testing.T) {
	ends := 0
	w := &cloudFileReadWriter{end: func() error { ends++; return errCloudFileReadEnded }}
	firstClose := w.Close()
	secondClose := w.Close()
	if firstClose != errCloudFileReadEnded || secondClose != errCloudFileReadEnded || ends != 1 {
		t.Fatal("cleanup failure lost or repeated")
	}
	if n, err := w.Write([]byte("private")); n != 0 || err != errCloudFileReadEnded {
		t.Fatal("closed transfer resumed")
	}
}
