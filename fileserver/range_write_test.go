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

func TestSingleRangeBoundaries(t *testing.T) {
	for _, fixture := range []struct {
		header string
		size uint64
		start uint64
		end uint64
		ok bool
	}{
		{"bytes=0-9", 10, 0, 9, true},
		{"bytes=3-", 10, 3, 9, true},
		{"bytes=-3", 10, 7, 9, true},
		{"bytes=-20", 10, 0, 9, true},
		{"bytes=0-20", 10, 0, 9, true},
		{"bytes=10-", 10, 0, 0, false},
		{"bytes=0-0", 0, 0, 0, false},
		{"bytes=-0", 10, 0, 0, false},
		{"items=0-1", 10, 0, 0, false},
		{"0-1", 10, 0, 0, false},
		{"bytes=0-1,3-4", 10, 0, 0, false},
		{"bytes=4-3", 10, 0, 0, false},
	} {
		start, end, ok := parseRange(fixture.header, fixture.size)
		if ok != fixture.ok || (ok && (start != fixture.start || end != fixture.end)) {
			t.Errorf("parseRange(%q,%d)=(%d,%d,%v); want (%d,%d,%v)",
				fixture.header, fixture.size, start, end, ok, fixture.start, fixture.end, fixture.ok)
		}
	}
}
