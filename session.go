package grequests

import (
	"context"
	"net/http"
)

// Session allows a user to make use of persistent cookies in between
// HTTP requests
type Session struct {
	// RequestOptions is global options
	RequestOptions *RequestOptions

	// HTTPClient is the client that we will use to request the resources
	HTTPClient *http.Client
}

// NewSession returns a session struct which enables can be used to maintain establish a persistent state with the
// server
// This function will set UseCookieJar to true as that is the purpose of using the session
func NewSession(ro *RequestOptions) *Session {
	if ro == nil {
		ro = &RequestOptions{}
	} else {
		// The session keeps its own copy - the caller's RequestOptions
		// is never written to
		ro = ro.clone()
	}

	ro.UseCookieJar = true

	return &Session{RequestOptions: ro, HTTPClient: BuildHTTPClient(*ro)}
}

// Combine session options and request options
// 1. UserAgent
// 2. Host
// 3. Auth
// 4. Headers
// 5. RedirectLimit
// 6. SensitiveHTTPHeaders
//
// The caller's RequestOptions is treated as read-only: the combined
// options are returned as a new struct.
func (s *Session) combineRequestOptions(ro *RequestOptions) *RequestOptions {
	if ro == nil {
		ro = &RequestOptions{}
	}

	combined := ro.clone()

	if combined.UserAgent == "" && s.RequestOptions.UserAgent != "" {
		combined.UserAgent = s.RequestOptions.UserAgent
	}

	if combined.Host == "" && s.RequestOptions.Host != "" {
		combined.Host = s.RequestOptions.Host
	}

	if combined.Auth == nil && s.RequestOptions.Auth != nil {
		combined.Auth = append([]string(nil), s.RequestOptions.Auth...)
	}

	if len(s.RequestOptions.Headers) > 0 || len(combined.Headers) > 0 {
		headers := make(map[string]string)
		for k, v := range s.RequestOptions.Headers {
			headers[k] = v
		}
		for k, v := range combined.Headers {
			headers[k] = v
		}
		combined.Headers = headers
	}

	// A non-zero redirect limit on the request overrides the session
	// default - zero falls back to RequestRedirectLimit later on
	if combined.RedirectLimit == 0 {
		combined.RedirectLimit = s.RequestOptions.RedirectLimit
	}

	// A non-nil sensitive header set on the request overrides the session
	// default - an explicitly empty set means no headers are stripped
	if combined.SensitiveHTTPHeaders == nil && s.RequestOptions.SensitiveHTTPHeaders != nil {
		combined.SensitiveHTTPHeaders = make(map[string]struct{}, len(s.RequestOptions.SensitiveHTTPHeaders))
		for k, v := range s.RequestOptions.SensitiveHTTPHeaders {
			combined.SensitiveHTTPHeaders[k] = v
		}
	}

	return combined
}

// Get takes 2 parameters and returns a Response Struct. These two options are:
//  1. A URL
//  2. A RequestOptions struct
//
// If you do not intend to use the `RequestOptions` you can just pass nil
// A new session is created by calling NewSession with a request options struct
func (s *Session) Get(ctx context.Context, url string, ro *RequestOptions) (*Response, error) {
	ro = s.combineRequestOptions(ro)
	if ctx != nil {
		ro.Context = ctx
	}
	return doSessionRequest("GET", url, ro, s.HTTPClient)
}

// Put takes 2 parameters and returns a Response struct. These two options are:
//  1. A URL
//  2. A RequestOptions struct
//
// If you do not intend to use the `RequestOptions` you can just pass nil
// A new session is created by calling NewSession with a request options struct
func (s *Session) Put(ctx context.Context, url string, ro *RequestOptions) (*Response, error) {
	ro = s.combineRequestOptions(ro)
	if ctx != nil {
		ro.Context = ctx
	}
	return doSessionRequest("PUT", url, ro, s.HTTPClient)
}

// Patch takes 2 parameters and returns a Response struct. These two options are:
//  1. A URL
//  2. A RequestOptions struct
//
// If you do not intend to use the `RequestOptions` you can just pass nil
// A new session is created by calling NewSession with a request options struct
func (s *Session) Patch(ctx context.Context, url string, ro *RequestOptions) (*Response, error) {
	ro = s.combineRequestOptions(ro)
	if ctx != nil {
		ro.Context = ctx
	}
	return doSessionRequest("PATCH", url, ro, s.HTTPClient)
}

// Delete takes 2 parameters and returns a Response struct. These two options are:
//  1. A URL
//  2. A RequestOptions struct
//
// If you do not intend to use the `RequestOptions` you can just pass nil
// A new session is created by calling NewSession with a request options struct
func (s *Session) Delete(ctx context.Context, url string, ro *RequestOptions) (*Response, error) {
	ro = s.combineRequestOptions(ro)
	if ctx != nil {
		ro.Context = ctx
	}
	return doSessionRequest("DELETE", url, ro, s.HTTPClient)
}

// Post takes 2 parameters and returns a Response channel. These two options are:
//  1. A URL
//  2. A RequestOptions struct
//
// If you do not intend to use the `RequestOptions` you can just pass nil
// A new session is created by calling NewSession with a request options struct
func (s *Session) Post(ctx context.Context, url string, ro *RequestOptions) (*Response, error) {
	ro = s.combineRequestOptions(ro)
	if ctx != nil {
		ro.Context = ctx
	}
	return doSessionRequest("POST", url, ro, s.HTTPClient)
}

// Head takes 2 parameters and returns a Response channel. These two options are:
//  1. A URL
//  2. A RequestOptions struct
//
// If you do not intend to use the `RequestOptions` you can just pass nil
// A new session is created by calling NewSession with a request options struct
func (s *Session) Head(ctx context.Context, url string, ro *RequestOptions) (*Response, error) {
	ro = s.combineRequestOptions(ro)
	if ctx != nil {
		ro.Context = ctx
	}
	return doSessionRequest("HEAD", url, ro, s.HTTPClient)
}

// Options takes 2 parameters and returns a Response struct. These two options are:
//  1. A URL
//  2. A RequestOptions struct
//
// If you do not intend to use the `RequestOptions` you can just pass nil
// A new session is created by calling NewSession with a request options struct
func (s *Session) Options(ctx context.Context, url string, ro *RequestOptions) (*Response, error) {
	ro = s.combineRequestOptions(ro)
	if ctx != nil {
		ro.Context = ctx
	}
	return doSessionRequest("OPTIONS", url, ro, s.HTTPClient)
}

// CloseIdleConnections closes the idle connections that a session client may make use of
func (s *Session) CloseIdleConnections() {
	s.HTTPClient.Transport.(*http.Transport).CloseIdleConnections()
}
