package grequests

import (
	"errors"
	"io"
	"net/http"
	"runtime"
	"time"
)

const (
	localUserAgent = "GRequests/0.10"

	// Default value for net.Dialer Timeout
	dialTimeout = 30 * time.Second

	// Default value for net.Dialer KeepAlive
	dialKeepAlive = 30 * time.Second

	// Default value for http.Transport TLSHandshakeTimeout
	tlsHandshakeTimeout = 10 * time.Second

	// Default value for Request Timeout
	requestTimeout = 90 * time.Second
)

var (
	// ErrRedirectLimitExceeded is the error returned when the request responded
	// with too many redirects
	ErrRedirectLimitExceeded = errors.New("grequests: Request exceeded redirect count")

	// RequestRedirectLimit is a tunable variable that specifies how many times we can
	// redirect in response to a redirect. This is the global variable, if you
	// wish to set this on a request by request basis, set it within the
	// `RequestOptions` structure
	RequestRedirectLimit = 30

	// RequestSensitiveHTTPHeaders is a map of sensitive HTTP headers that a user
	// doesn't want passed on a redirect. This is the global variable, if you
	// wish to set this on a request by request basis, set it within the
	// `RequestOptions` structure
	RequestSensitiveHTTPHeaders = map[string]struct{}{
		"Www-Authenticate":    {},
		"Authorization":       {},
		"Proxy-Authorization": {},
	}
)

// XMLCharDecoder is a helper type that takes a stream of bytes (not encoded in
// UTF-8) and returns a reader that encodes the bytes into UTF-8. This is done
// because Go's XML library only supports XML encoded in UTF-8
type XMLCharDecoder func(charset string, input io.Reader) (io.Reader, error)

// redirectCheckFunc builds the CheckRedirect function for a single request.
// The redirect policy of every request is independent: the limit and the
// sensitive header set are captured here (never written back into ro) so
// requests sharing a client cannot affect each other. If next (a
// user-provided CheckRedirect) is supplied, it is only called after the
// library policy has accepted the redirect.
func redirectCheckFunc(ro *RequestOptions, next func(req *http.Request, via []*http.Request) error) func(req *http.Request, via []*http.Request) error {
	limit := ro.RedirectLimit
	if limit == 0 {
		limit = RequestRedirectLimit
	}

	sensitiveHTTPHeaders := ro.SensitiveHTTPHeaders
	if sensitiveHTTPHeaders == nil {
		sensitiveHTTPHeaders = RequestSensitiveHTTPHeaders
	}

	return func(req *http.Request, via []*http.Request) error {

		if ro.RedirectLimit < 0 {
			return http.ErrUseLastResponse
		}

		if len(via) >= limit {
			return ErrRedirectLimitExceeded
		}

		for k, vv := range via[0].Header {
			// Is this a sensitive header?
			if _, found := sensitiveHTTPHeaders[k]; found {
				continue
			}

			// net/http has already carried this header onto the redirect
			// request - copying it again would grow the values on every hop
			if _, found := req.Header[k]; found {
				continue
			}

			for _, v := range vv {
				req.Header.Add(k, v)
			}
		}

		if next != nil {
			return next(req, via)
		}

		return nil
	}
}

// withRedirectPolicy returns a shallow copy of client with the per-request
// redirect policy installed. The copy shares the Transport, Jar and Timeout
// of the original client, so connection pools and cookie state are still
// reused, while the original client (including http.DefaultClient) is never
// modified.
func withRedirectPolicy(client *http.Client, ro *RequestOptions) *http.Client {
	clientCopy := *client
	clientCopy.CheckRedirect = redirectCheckFunc(ro, client.CheckRedirect)
	return &clientCopy
}

// EnsureTransporterFinalized will ensure that when the HTTP client is GCed
// the runtime will close the idle connections (so that they won't leak)
// this function was adopted from Hashicorp's go-cleanhttp package
func EnsureTransporterFinalized(httpTransport *http.Transport) {
	runtime.SetFinalizer(&httpTransport, func(transportInt **http.Transport) {
		(*transportInt).CloseIdleConnections()
	})
}

// EnsureResponseFinalized will ensure that when the Response is GCed
// the request body is closed so we aren't leaking fds
// func EnsureResponseFinalized(httpResp *Response) {
// 	runtime.SetFinalizer(&httpResp, func(httpResponseInt **Response) {
// 		(*httpResponseInt).RawResponse.Body.Close()
// 	})
// }
// This will come back in 1.0
