package main

import (
	"errors"
	"io"
	"testing"
)

type rangeFixtureWriter struct {
	n int
	err error
}

func (w rangeFixtureWriter) Write(data []byte) (int, error) {
	return w.n, w.err
}

func TestRangeChunkRequiresCompleteWrite(t *testing.T) {
	failure := errors.New("fixture disconnect")
	for _, fixture := range []struct {
		writer rangeFixtureWriter
		want error
	}{
		{rangeFixtureWriter{3, nil}, nil},
		{rangeFixtureWriter{2, nil}, io.ErrShortWrite},
		{rangeFixtureWriter{0, failure}, failure},
		{rangeFixtureWriter{2, failure}, failure},
	} {
		if err := writeRangeChunk(fixture.writer, []byte("abc")); err != fixture.want {
			t.Fatalf("range write error = %v; want %v", err, fixture.want)
		}
	}
}
