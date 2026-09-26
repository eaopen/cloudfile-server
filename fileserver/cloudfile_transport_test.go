package main

import (
	"net"
	"net/http/httptest"
	"testing"
)

func TestManagedReadRequiresActualTrustedTLSProxyPeer(t *testing.T) {
	_, trusted, _ := net.ParseCIDR("192.0.2.7/32")
	for _, fixture := range []struct {
		peer, proto string
		allowed     bool
	}{
		{"192.0.2.7:1234", "https", true},
		{"192.0.2.8:1234", "https", false},
		{"192.0.2.7:1234", "http", false},
		{"192.0.2.7:1234", "https,http", false},
		{"192.0.2.7", "https", false},
	} {
		r := httptest.NewRequest("GET", "http://fixture.invalid/read", nil)
		r.RemoteAddr = fixture.peer
		r.Header.Set("X-Forwarded-Proto", fixture.proto)
		r.Header.Set("X-Forwarded-For", "192.0.2.7")
		if cloudFileSecureTransport(r, []*net.IPNet{trusted}) != fixture.allowed {
			t.Errorf("unexpected transport proof for peer %s", fixture.peer)
		}
		if cloudFileSecureTransport(r, nil) {
			t.Fatal("unconfigured proxy trusted")
		}
		r.Header.Add("X-Forwarded-Proto", "https")
		if cloudFileSecureTransport(r, []*net.IPNet{trusted}) {
			t.Fatal("duplicate protocol accepted")
		}
	}
}
