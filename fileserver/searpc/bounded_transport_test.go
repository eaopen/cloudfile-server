package searpc

import (
	"encoding/binary"
	"io"
	"net"
	"path/filepath"
	"testing"
	"time"
)

func boundedFixtureListener(t *testing.T) (net.Listener, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "rpc.sock")
	if len(path) > 100 {
		t.Skip("Unix fixture path too long; use a short TMPDIR")
	}
	listener, err := net.Listen("unix", path)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { listener.Close() })
	return listener, path
}

func readFixtureRequest(conn net.Conn) error {
	header := make([]byte, 4)
	if _, err := io.ReadFull(conn, header); err != nil {
		return err
	}
	returnLength := binary.LittleEndian.Uint32(header)
	if returnLength > 65536 {
		return io.ErrShortBuffer
	}
	_, err := io.CopyN(io.Discard, conn, int64(returnLength))
	return err
}

func TestBoundedTransportDiscardsTimedOutConnection(t *testing.T) {
	listener, path := boundedFixtureListener(t)
	done := make(chan error, 1)
	go func() {
		first, err := listener.Accept()
		if err != nil {
			done <- err
			return
		}
		first.SetDeadline(time.Now().Add(2 * time.Second))
		if err = readFixtureRequest(first); err == nil {
			// Wait for EOF caused by the actual client's deadline discard.
			_, err = io.Copy(io.Discard, first)
		}
		first.Close()
		if err != nil {
			done <- err
			return
		}
		second, err := listener.Accept()
		if err != nil {
			done <- err
			return
		}
		defer second.Close()
		second.SetDeadline(time.Now().Add(2 * time.Second))
		if err = readFixtureRequest(second); err != nil {
			done <- err
			return
		}
		body := []byte(`{"ret":0}`)
		header := make([]byte, 4)
		binary.LittleEndian.PutUint32(header, uint32(len(body)))
		if _, err = second.Write(append(header, body...)); err != nil {
			done <- err
			return
		}
		done <- nil
	}()
	client := Init(path, "fixture", 1)
	if _, err := client.CallWithTimeout(100*time.Millisecond, "fixture"); err == nil {
		t.Fatal("slow reply accepted")
	}
	if len(client.pool) != 0 {
		t.Fatal("timed out transport returned to pool")
	}
	value, err := client.CallWithTimeout(time.Second, "fixture")
	if err != nil || value != float64(0) {
		t.Fatalf("fresh exchange failed: %v", err)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("fixture did not close")
	}
	select {
	case conn := <-client.pool:
		conn.Close()
	default:
	}
}

func TestBoundedTransportRejectsOversizeBeforeBodyAllocation(t *testing.T) {
	listener, path := boundedFixtureListener(t)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		conn.SetDeadline(time.Now().Add(time.Second))
		if readFixtureRequest(conn) != nil {
			return
		}
		header := make([]byte, 4)
		binary.LittleEndian.PutUint32(header, 0xffffffff)
		conn.Write(header)
	}()
	client := Init(path, "fixture", 1)
	if _, err := client.CallWithTimeout(time.Second, "fixture"); err == nil {
		t.Fatal("oversized response accepted")
	}
	if len(client.pool) != 0 {
		t.Fatal("malformed transport returned to pool")
	}
}
