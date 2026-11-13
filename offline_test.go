package opengraph

import (
	"fmt"
	"net/http"
)

type testLocalTransport struct{ next http.RoundTripper }

func (t testLocalTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	switch r.URL.Hostname() {
	case "localhost", "127.0.0.1", "::1":
		return t.next.RoundTrip(r)
	}
	return nil, fmt.Errorf("offline support rejects external request to %s", r.URL.Hostname())
}
func init() { http.DefaultTransport = testLocalTransport{http.DefaultTransport} }
