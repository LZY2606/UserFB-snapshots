package requests

import (
	"errors"
	"fmt"
	"net/http"
	"net/netip"
	"net/url"
	"slices"
	"strings"
)

// SensitiveHeaders declares headers that must not leak across origins
// when the default http.Client redirect handling follows a redirect chain.
// It is opt-in: a Builder that never calls SensitiveHeaders
// keeps the standard net/http behavior of copying custom headers
// to every redirect target.
//
// The policy guards the authority boundary of a redirect chain;
// it does not disable redirect following.
// The declared headers are sent with the initial request
// and with every redirect that stays on the initial origin.
// The first time a redirect crosses to another origin,
// the declared headers are stripped from the candidate request,
// and they stay stripped for the rest of the chain,
// even if a later redirect returns to the initial origin.
// Headers that were not declared keep following
// the standard net/http redirect rules.
//
// Two URLs share an origin when they have the same scheme,
// host, and effective port.
// The scheme and ASCII DNS hosts are compared case-insensitively.
// A missing port means 80 for http and 443 for https,
// so http://example.com and http://example.com:80 share an origin,
// but a different port, a different scheme (including an HTTPS to HTTP downgrade),
// a subdomain, or a trailing dot in the host all count as a cross-origin redirect.
// IPv6 literal hosts are compared by address value,
// so http://[::1] and http://[0:0:0:0:0:0:0:1] share an origin.
// Only syntactically valid HTTP(S) URLs can establish an origin;
// a redirect to a URL whose origin cannot be determined
// fails with an error before the request is sent.
//
// Header names are matched case-insensitively, and all values of a
// declared header are stripped. Calls to SensitiveHeaders accumulate:
// each call adds names to the set (deduplicated case-insensitively),
// and a call with no names leaves the set unchanged.
// The names are copied, so later mutation of the caller's slice
// does not affect the Builder.
//
// The policy applies to both [Builder.Fetch] and [Builder.Do],
// including when [Builder.Transport] is set.
// The http.Client and its CheckRedirect func and Transport are never modified;
// each request works on a shallow copy,
// so a Client shared with other code keeps its prior behavior,
// and concurrent requests made with the same Builder
// each track their own "left the initial origin" state.
// An existing Client.CheckRedirect is still consulted for every hop
// and observes the already-sanitized candidate request;
// if it re-adds a declared header, the header is stripped again before sending.
// If the Client has no CheckRedirect of its own,
// the standard library default of at most 10 redirects still applies.
//
// Note that the standard library already strips Authorization and Cookie
// headers on some cross-domain redirects; SensitiveHeaders exists because
// that built-in handling does not cover caller-defined headers
// such as X-Service-Token:
//
//	err := requests.
//		URL("https://service.internal/api").
//		Header("X-Service-Token", token).
//		SensitiveHeaders("X-Service-Token").
//		ToJSON(&v).
//		Fetch(ctx)
func (rb *Builder) SensitiveHeaders(names ...string) *Builder {
	for _, name := range names {
		key := http.CanonicalHeaderKey(name)
		if !slices.Contains(rb.sensitiveHeaders, key) {
			rb.sensitiveHeaders = append(rb.sensitiveHeaders, key)
		}
	}
	return rb
}

// origin identifies the origin of a URL
// for the purpose of deciding whether a redirect
// crosses an authority boundary.
type origin struct {
	scheme string
	host   string
	port   string
}

// originOf derives the origin of u.
// It returns an error if u is not an absolute HTTP(S) URL
// whose origin identity can be established.
func originOf(u *url.URL) (origin, error) {
	if u == nil {
		return origin{}, errors.New("requests: cannot determine origin of nil URL")
	}
	scheme := strings.ToLower(u.Scheme)
	var defaultPort string
	switch scheme {
	case "http":
		defaultPort = "80"
	case "https":
		defaultPort = "443"
	default:
		return origin{}, fmt.Errorf("requests: cannot determine origin of URL with scheme %q", u.Scheme)
	}
	host := u.Hostname()
	if host == "" {
		return origin{}, fmt.Errorf("requests: cannot determine origin of URL %q with no host", u.Redacted())
	}
	if addr, err := netip.ParseAddr(host); err == nil {
		// Compare IP literal hosts by address value.
		host = addr.String()
	} else {
		host = strings.ToLower(host)
	}
	port := u.Port()
	if port == "" {
		port = defaultPort
	}
	return origin{scheme, host, port}, nil
}

// sensitiveRedirect enforces a Builder's sensitive header policy
// over the course of a single redirect chain.
// A fresh sensitiveRedirect is used for each request,
// so concurrent requests do not share state.
type sensitiveRedirect struct {
	names  []string // canonical header keys
	origin origin   // origin of the initial request
	left   bool     // chain has left the initial origin
	next   CheckRedirectPolicy
}

// checkRedirect is suitable for use as http.Client.CheckRedirect.
// The candidate request it receives is the request the client will send,
// so stripping headers from it (after next runs, as well as before)
// keeps them off the wire.
func (sr *sensitiveRedirect) checkRedirect(req *http.Request, via []*http.Request) error {
	o, err := originOf(req.URL)
	if err != nil {
		return err
	}
	if o != sr.origin {
		// Once the chain has left the initial origin,
		// it never regains the sensitive headers.
		sr.left = true
	}
	if sr.left {
		stripHeaders(req.Header, sr.names)
	}
	if sr.next != nil {
		err = sr.next(req, via)
	} else if len(via) >= 10 {
		// Preserve the standard library default policy.
		err = errors.New("stopped after 10 redirects")
	}
	if err != nil {
		return err
	}
	if sr.left {
		// The redirect policy callback may have re-added
		// the sensitive headers; strip them again before sending.
		stripHeaders(req.Header, sr.names)
	}
	return nil
}

// stripHeaders removes all values of the named headers from h.
// names must already be in canonical form,
// but the keys of h are matched case-insensitively.
func stripHeaders(h http.Header, names []string) {
	for key := range h {
		if slices.Contains(names, http.CanonicalHeaderKey(key)) {
			delete(h, key)
		}
	}
}
