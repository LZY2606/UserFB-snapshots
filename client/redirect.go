package client

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"sync"
)

// RequestDeniedError reports a request rejected by scraping policy
// (AllowedDomains or robots.txt) rather than by a download failure.
// Policy rejections are final: they are not retried.
type RequestDeniedError struct {
	// URL is the address that was refused.
	URL string
	// Reason explains the refusal: a disallowed domain or robots.txt rules.
	Reason string
}

func (e *RequestDeniedError) Error() string {
	return fmt.Sprintf("request denied: %s (%s)", e.URL, e.Reason)
}

// HostLimiter limits concurrent downloads per host.
// Hosts are compared as URL.Host values, so different ports are different sites.
type HostLimiter interface {
	// Acquire reserves a slot for host, waiting until one is free or ctx is done.
	Acquire(ctx context.Context, host string) error
	// Release returns a slot previously reserved for host.
	Release(host string)
}

// LeaseTransport applies a HostLimiter to every download it starts.
// The lease is held from the moment the request is sent until its response
// body is closed, so a slot covers reading the body and not just the headers.
type LeaseTransport struct {
	Base    http.RoundTripper
	Limiter HostLimiter
}

func (t *LeaseTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	base := t.Base
	if base == nil {
		base = http.DefaultTransport
	}
	// Supporting probes such as robots.txt must not consume a page's lease.
	// Otherwise a page waiting for its own host slot could deadlock against
	// the robots.txt probe it just triggered.
	if t.Limiter == nil || IsProbeRequest(req) {
		return base.RoundTrip(req)
	}

	host := req.URL.Host
	if err := t.Limiter.Acquire(req.Context(), host); err != nil {
		return nil, err
	}

	resp, err := base.RoundTrip(req)
	if err != nil {
		t.Limiter.Release(host)
		return nil, err
	}

	resp.Body = &leaseBody{ReadCloser: resp.Body, release: func() { t.Limiter.Release(host) }}
	return resp, nil
}

// leaseBody releases its lease once, when the response body is closed.
type leaseBody struct {
	io.ReadCloser
	once    sync.Once
	release func()
}

func (b *leaseBody) Close() error {
	err := b.ReadCloser.Close()
	b.once.Do(b.release)
	return err
}

type probeRequestKey struct{}

// MarkProbeRequest marks req as a supporting probe (such as robots.txt)
// that does not occupy per-host download leases.
func MarkProbeRequest(req *Request) {
	req.Request = req.Request.WithContext(context.WithValue(req.Request.Context(), probeRequestKey{}, true))
}

// IsProbeRequest reports whether req was marked with MarkProbeRequest.
func IsProbeRequest(req *http.Request) bool {
	v, _ := req.Context().Value(probeRequestKey{}).(bool)
	return v
}
