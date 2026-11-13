package sling

import (
	"fmt"
	"net"
	"net/http"
)

type testLocalTransport struct{ next http.RoundTripper }

func (t testLocalTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	if r.URL.Hostname() == "::" || r.URL.Hostname() == "0.0.0.0" {
		next := r.Clone(r.Context())
		u := *r.URL
		host := "127.0.0.1"
		if r.URL.Hostname() == "::" {
			host = "::1"
		}
		u.Host = net.JoinHostPort(host, r.URL.Port())
		next.URL = &u
		return t.next.RoundTrip(next)
	}
	switch r.URL.Hostname() {
	case "localhost", "127.0.0.1", "::1":
		return t.next.RoundTrip(r)
	}
	return nil, fmt.Errorf("offline support rejects external request to %s", r.URL.Hostname())
}
func init() { http.DefaultTransport = testLocalTransport{http.DefaultTransport} }
