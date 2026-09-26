package main

import (
	"context"
	"errors"
	"io"
	"time"
)

const cloudFileReadBlock = 65536

var errCloudFileReadEnded = errors.New("guarded read transfer unavailable")

// No ReadFrom optimization: all bytes must pass the guarded Write path.
// The host still owns HTTP status/headers and may not reuse this writer across
// requests. Previously accepted socket-buffer bytes cannot be recalled.
type cloudFileReadWriter struct {
	writer     io.Writer
	context    context.Context
	check      func() error
	failed     error
	end        func() error
	closed     bool
	closeError error
}

func newCloudFileReadWriter(writer io.Writer, ctx context.Context, token string) (*cloudFileReadWriter, error) {
	if writer == nil || ctx == nil || !canonicalTicketUUID(token) || rpcclient == nil {
		return nil, errCloudFileReadEnded
	}
	return &cloudFileReadWriter{writer: writer, context: ctx, check: func() error {
		value, err := rpcclient.CallWithTimeout(5*time.Second, "seafile_cloudfile_check_read_transfer", token)
		// Actual searpc JSON decoder returns numbers as float64. Do not coerce
		// strings, booleans, nil or nonzero statuses into an authorization grant.
		status, ok := value.(float64)
		if err != nil || !ok || status != 0 {
			return errCloudFileReadEnded
		}
		return nil
	}, end: func() error {
		value, err := rpcclient.CallWithTimeout(5*time.Second, "seafile_cloudfile_end_read_transfer", token)
		status, ok := value.(float64)
		if err != nil || !ok || status != 0 {
			return errCloudFileReadEnded
		}
		return nil
	}}, nil
}

// The host defers Close immediately after construction, including on failures.
// It does not close the caller-owned HTTP writer or turn cleanup into success.
func (w *cloudFileReadWriter) Close() error {
	if !w.closed {
		w.closed = true
		if w.end != nil {
			w.closeError = w.end()
		}
	}
	return w.closeError
}

func (w *cloudFileReadWriter) Write(content []byte) (int, error) {
	if w.closed {
		return 0, errCloudFileReadEnded
	}
	if w.failed != nil {
		return 0, w.failed
	}
	written := 0
	for len(content) > 0 {
		if err := w.context.Err(); err != nil {
			w.failed = err
			return written, err
		}
		if err := w.check(); err != nil {
			w.failed = err
			return written, err
		}
		if err := w.context.Err(); err != nil {
			w.failed = err
			return written, err
		}
		length := len(content)
		if length > cloudFileReadBlock {
			length = cloudFileReadBlock
		}
		n, err := w.writer.Write(content[:length])
		if n < 0 || n > length {
			n = 0
			err = io.ErrShortWrite
		}
		written += n
		if err == nil && n != length {
			err = io.ErrShortWrite
		}
		if err != nil {
			w.failed = err
			return written, err
		}
		content = content[length:]
	}
	return written, nil
}
