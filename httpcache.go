// Package httpcache provides a http.RoundTripper implementation that works as a
// mostly RFC-compliant cache for http responses.
//
// It is only suitable for use as a 'private' cache (i.e. for a web-browser or an API-client
// and not for a shared proxy).
package httpcache

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"net/http"
	"net/http/httputil"
	"strings"
	"sync"
	"time"
)

const (
	stale = iota
	fresh
	transparent
	// XFromCache is the header added to responses that are returned from the cache
	XFromCache = "X-From-Cache"
)

// A Cache interface is used by the Transport to store and retrieve responses.
type Cache interface {
	// Get returns the []byte representation of a cached response and a bool
	// set to true if the value isn't empty
	Get(key string) (responseBytes []byte, ok bool)
	// Set stores the []byte representation of a response against a key
	Set(key string, responseBytes []byte)
	// Delete removes the value associated with the key
	Delete(key string)
}

// cacheKey returns the cache key for req.
func cacheKey(req *http.Request) string {
	if req.Method == http.MethodGet {
		return req.URL.String()
	} else {
		return req.Method + " " + req.URL.String()
	}
}

// CachedResponse returns the cached http.Response for req if present, and nil
// otherwise.
func CachedResponse(c Cache, req *http.Request) (resp *http.Response, err error) {
	cachedVal, ok := c.Get(cacheKey(req))
	if !ok {
		return
	}

	b := bytes.NewBuffer(cachedVal)
	return http.ReadResponse(bufio.NewReader(b), req)
}

// MemoryCache is an implemtation of Cache that stores responses in an in-memory map.
type MemoryCache struct {
	mu    sync.RWMutex
	items map[string][]byte
}

// Get returns the []byte representation of the response and true if present, false if not
func (c *MemoryCache) Get(key string) (resp []byte, ok bool) {
	c.mu.RLock()
	resp, ok = c.items[key]
	c.mu.RUnlock()
	return resp, ok
}

// Set saves response resp to the cache with key
func (c *MemoryCache) Set(key string, resp []byte) {
	c.mu.Lock()
	c.items[key] = resp
	c.mu.Unlock()
}

// Delete removes key from the cache
func (c *MemoryCache) Delete(key string) {
	c.mu.Lock()
	delete(c.items, key)
	c.mu.Unlock()
}

// NewMemoryCache returns a new Cache that will store items in an in-memory map
func NewMemoryCache() *MemoryCache {
	c := &MemoryCache{items: map[string][]byte{}}
	return c
}

// Transport is an implementation of http.RoundTripper that will return values from a cache
// where possible (avoiding a network request) and will additionally add validators (etag/if-modified-since)
// to repeated requests allowing servers to return 304 / Not Modified
type Transport struct {
	// The RoundTripper interface actually used to make requests
	// If nil, http.DefaultTransport is used
	Transport http.RoundTripper
	Cache     Cache
	// If true, responses returned from the cache will be given an extra header, X-From-Cache
	MarkCachedResponses bool

	// coordMu protects coords. It is never held while the downstream
	// RoundTripper is called or while response bodies are read or closed.
	coordMu sync.Mutex
	// coords holds one entry per cache key that currently has at least one
	// in-flight GET/HEAD request. Entries are created on demand and removed
	// once the last in-flight request (including its response Body) is done.
	coords map[string]*keyCoord
}

// keyCoord coordinates update attempts for a single cache key.
//
// generation is the state version of the key. Every successful Set or Delete
// bumps it, even when the stored bytes are identical to the previous value.
// An update attempt is allowed to commit only while the generation it
// observed before sending its request is still current.
type keyCoord struct {
	generation uint64
	inFlight   int
}

// updateAttempt represents one downstream request as an attempt to update the
// cache key based on the state observed immediately before the request left.
type updateAttempt struct {
	t    *Transport
	key  string
	kc   *keyCoord
	seen uint64

	releaseOnce sync.Once
}

// begin registers an in-flight attempt for key and captures the generation
// observed before the request is sent.
func (t *Transport) begin(key string) *updateAttempt {
	t.coordMu.Lock()
	defer t.coordMu.Unlock()
	if t.coords == nil {
		t.coords = map[string]*keyCoord{}
	}
	kc, ok := t.coords[key]
	if !ok {
		kc = &keyCoord{}
		t.coords[key] = kc
	}
	kc.inFlight++
	return &updateAttempt{t: t, key: key, kc: kc, seen: kc.generation}
}

// release marks the attempt finished and reclaims the coordinator entry when
// no in-flight attempts remain for the key.
func (a *updateAttempt) release() {
	a.releaseOnce.Do(func() {
		a.t.coordMu.Lock()
		defer a.t.coordMu.Unlock()
		if kc, ok := a.t.coords[a.key]; ok && kc == a.kc {
			kc.inFlight--
			if kc.inFlight == 0 {
				delete(a.t.coords, a.key)
			}
		}
	})
}

// current reports whether the state observed at departure is still the
// current state of the key.
func (a *updateAttempt) current() bool {
	a.t.coordMu.Lock()
	defer a.t.coordMu.Unlock()
	return a.kc.generation == a.seen
}

// trySet stores responseBytes only when the observed state is still current,
// atomically advancing the generation. It returns whether the commit won.
func (a *updateAttempt) trySet(responseBytes []byte) bool {
	a.t.coordMu.Lock()
	defer a.t.coordMu.Unlock()
	if a.kc.generation != a.seen {
		return false
	}
	a.t.Cache.Set(a.key, responseBytes)
	a.kc.generation++
	return true
}

// tryDelete removes the key only when the observed state is still current,
// atomically advancing the generation. Deleting a missing key still creates
// a new state.
func (a *updateAttempt) tryDelete() bool {
	a.t.coordMu.Lock()
	defer a.t.coordMu.Unlock()
	if a.kc.generation != a.seen {
		return false
	}
	a.t.Cache.Delete(a.key)
	a.kc.generation++
	return true
}

// NewTransport returns a new Transport with the
// provided Cache implementation and MarkCachedResponses set to true
func NewTransport(c Cache) *Transport {
	return &Transport{Cache: c, MarkCachedResponses: true}
}

// Client returns an *http.Client that caches responses.
func (t *Transport) Client() *http.Client {
	return &http.Client{Transport: t}
}

// varyMatches will return false unless all of the cached values for the headers listed in Vary
// match the new request
func varyMatches(cachedResp *http.Response, req *http.Request) bool {
	for _, header := range headerAllCommaSepValues(cachedResp.Header, "vary") {
		header = http.CanonicalHeaderKey(header)
		if header != "" && req.Header.Get(header) != cachedResp.Header.Get("X-Varied-"+header) {
			return false
		}
	}
	return true
}

// hasCallerConditional reports whether the caller supplied its own
// If-None-Match or If-Modified-Since header. Header names are matched
// case-insensitively; a present field, even with an empty value, counts.
func hasCallerConditional(h http.Header) bool {
	for name := range h {
		switch http.CanonicalHeaderKey(name) {
		case "If-None-Match", "If-Modified-Since":
			return true
		}
	}
	return false
}

// withAutoValidators returns a clone of req carrying validators taken from
// cachedResp, or nil if there is nothing to add. The original request and its
// header maps are never modified.
func withAutoValidators(req *http.Request, cachedResp *http.Response) *http.Request {
	var req2 *http.Request
	etag := cachedResp.Header.Get("etag")
	if etag != "" && req.Header.Get("etag") == "" {
		req2 = cloneRequest(req)
		req2.Header.Set("if-none-match", etag)
	}
	lastModified := cachedResp.Header.Get("last-modified")
	if lastModified != "" && req.Header.Get("last-modified") == "" {
		if req2 == nil {
			req2 = cloneRequest(req)
		}
		req2.Header.Set("if-modified-since", lastModified)
	}
	return req2
}

// RoundTrip takes a Request and returns a Response
//
// If there is a fresh Response already in cache, then it will be returned without connecting to
// the server.
//
// If there is a stale Response, then any validators it contains will be set on the new request
// to give the server a chance to respond with NotModified. If this happens, then the cached Response
// will be returned.
//
// If the request itself carries If-None-Match or If-Modified-Since, those
// conditions belong to the caller: they are forwarded untouched, a fresh
// cache entry is never served instead, and a 304 from the origin is returned
// to the caller verbatim instead of being merged with a cached entity.
func (t *Transport) RoundTrip(req *http.Request) (resp *http.Response, err error) {
	cacheKey := cacheKey(req)
	cacheable := (req.Method == "GET" || req.Method == "HEAD") && req.Header.Get("range") == ""
	if !cacheable {
		// Need to invalidate an existing value; non-cacheable methods take no
		// part in per-key update coordination.
		t.Cache.Delete(cacheKey)
		return t.downstream().RoundTrip(req)
	}

	attempt := t.begin(cacheKey)

	cachedResp, cacheErr := CachedResponse(t.Cache, req)
	callerConditional := hasCallerConditional(req.Header)

	downstreamReq := req
	autoRevalidate := false
	if cachedResp != nil && cacheErr == nil {
		if t.MarkCachedResponses {
			cachedResp.Header.Set(XFromCache, "1")
		}
		// Caller-supplied conditions are never answered from the cache and
		// never augmented with cached validators.
		if !callerConditional && varyMatches(cachedResp, req) {
			switch getFreshness(cachedResp.Header, req.Header) {
			case fresh:
				attempt.release()
				return cachedResp, nil
			case stale:
				if req2 := withAutoValidators(req, cachedResp); req2 != nil {
					downstreamReq = req2
					autoRevalidate = true
				}
			}
		}
	} else if _, ok := parseCacheControl(req.Header)["only-if-cached"]; ok {
		// No cached value available and the caller forbids a network request.
		attempt.release()
		return newGatewayTimeoutResponse(req), nil
	}

	resp, err = t.downstream().RoundTrip(downstreamReq)
	if err != nil {
		// Explicit caller conditions, like any request without a usable
		// snapshot, surface the network error verbatim: no stale-if-error
		// fallback, no cache deletion.
		if cachedResp != nil && cacheErr == nil && !callerConditional &&
			req.Method == "GET" && canStaleOnError(cachedResp.Header, req.Header) &&
			attempt.current() {
			attempt.release()
			return cachedResp, nil
		}
		attempt.release()
		return nil, err
	}

	if resp.StatusCode == http.StatusNotModified {
		// A 304 may be synthesized into the cached 200 only when this
		// Transport itself generated conditional headers from that exact
		// snapshot. Caller conditions, Vary mismatches, no-cache requests and
		// requests without validators receive the 304 verbatim.
		if autoRevalidate && req.Method == "GET" {
			return t.mergedResponse(req, cachedResp, resp, attempt)
		}
		if !callerConditional {
			// Preserve the previous invalidation behavior for a 304 the
			// Transport cannot merge (an auto-revalidated HEAD, or a GET sent
			// without generated validators): the stale entry is dropped. The
			// delete still obeys the state rule. A caller-conditional 304
			// never touches the cache.
			attempt.tryDelete()
		}
		return t.passthrough(resp, attempt)
	}

	if resp.StatusCode >= 500 && cachedResp != nil && cacheErr == nil &&
		!callerConditional && req.Method == "GET" &&
		canStaleOnError(cachedResp.Header, req.Header) && attempt.current() {
		// stale-if-error: serve the snapshot taken at departure. The fallback
		// never writes the cache, and a snapshot that has since been replaced
		// or invalidated is not used.
		closeResponseBody(resp)
		attempt.release()
		return cachedResp, nil
	}

	if !canStore(parseCacheControl(req.Header), parseCacheControl(resp.Header)) {
		// no-store on the request or response: invalidate at header time. A
		// successful delete is a new state even if the key was absent, so an
		// older in-flight response can no longer resurrect it.
		attempt.tryDelete()
		return t.passthrough(resp, attempt)
	}

	if callerConditional && resp.StatusCode != http.StatusOK {
		// A caller's own conditional request only updates the cache through a
		// full 200. Its 304 is handled above; 4xx/5xx answers go back
		// verbatim without writing, deleting or refreshing anything.
		return t.passthrough(resp, attempt)
	}

	// Every storable response (of any status code) becomes a candidate that
	// commits at EOF under the state rule, preserving the previous behavior of
	// caching e.g. 404 responses.
	return t.storeResponse(req, resp, attempt)
}

// downstream returns the RoundTripper used for actual network requests.
func (t *Transport) downstream() http.RoundTripper {
	if t.Transport != nil {
		return t.Transport
	}
	return http.DefaultTransport
}

// passthrough returns resp to the caller without caching side effects,
// releasing the coordination attempt once the body is closed (or immediately
// when there is no body).
func (t *Transport) passthrough(resp *http.Response, attempt *updateAttempt) (*http.Response, error) {
	if resp.Body == nil {
		attempt.release()
		return resp, nil
	}
	resp.Body = &releaseReadCloser{rc: resp.Body, attempt: attempt}
	return resp, nil
}

// mergedResponse combines an auto-revalidation 304 with the cached snapshot
// and turns the snapshot into a cache candidate subject to the same EOF and
// state rules as a normal 200. The downstream 304 body is closed.
func (t *Transport) mergedResponse(req *http.Request, cachedResp, upstream *http.Response, attempt *updateAttempt) (*http.Response, error) {
	for _, header := range getEndToEndHeaders(upstream.Header) {
		// Copy the values so later caller mutations cannot reach the
		// downstream response and vice versa.
		cachedResp.Header[header] = append([]string(nil), upstream.Header[header]...)
	}
	closeResponseBody(upstream)
	return t.candidateResponse(req, cachedResp, attempt)
}

// storeResponse arranges for a cacheable response to be committed only after
// its body has been read to EOF and only while the departure state is still
// current.
func (t *Transport) storeResponse(req *http.Request, resp *http.Response, attempt *updateAttempt) (*http.Response, error) {
	meta := snapshotResponseMeta(resp)
	for _, varyKey := range headerAllCommaSepValues(resp.Header, "vary") {
		varyKey = http.CanonicalHeaderKey(varyKey)
		reqValue := req.Header.Get(varyKey)
		if reqValue != "" {
			meta.Header.Set("X-Varied-"+varyKey, reqValue)
		}
	}

	// HEAD responses (and bodyless responses) carry no entity to drain, so
	// the same state-guarded commit happens immediately.
	if req.Method == "HEAD" || resp.Body == nil {
		meta.Body = nil
		if respBytes, err := httputil.DumpResponse(meta, true); err == nil {
			attempt.trySet(respBytes)
		}
		return t.passthrough(resp, attempt)
	}

	resp.Body = &candidateReadCloser{
		rc:      resp.Body,
		attempt: attempt,
		meta:    meta,
		ctxDone: req.Context().Done(),
	}
	return resp, nil
}

// candidateResponse wraps an already materialized response (currently the
// cached snapshot merged with a revalidation 304) as a cache candidate.
func (t *Transport) candidateResponse(req *http.Request, resp *http.Response, attempt *updateAttempt) (*http.Response, error) {
	meta := snapshotResponseMeta(resp)
	if resp.Body == nil {
		meta.Body = nil
		if respBytes, err := httputil.DumpResponse(meta, true); err == nil {
			attempt.trySet(respBytes)
		}
		return t.passthrough(resp, attempt)
	}
	resp.Body = &candidateReadCloser{
		rc:      resp.Body,
		attempt: attempt,
		meta:    meta,
		ctxDone: req.Context().Done(),
	}
	return resp, nil
}

// snapshotResponseMeta makes an independent shallow copy of resp and a deep
// copy of its header map so that header mutations by the caller after
// RoundTrip returns cannot flow into cached metadata.
func snapshotResponseMeta(resp *http.Response) *http.Response {
	meta := new(http.Response)
	*meta = *resp
	meta.Header = resp.Header.Clone()
	return meta
}

func closeResponseBody(resp *http.Response) {
	if resp.Body != nil {
		resp.Body.Close()
	}
}

// ErrNoDateHeader indicates that the HTTP headers contained no Date header.
var ErrNoDateHeader = errors.New("no Date header")

// Date parses and returns the value of the Date header.
func Date(respHeaders http.Header) (date time.Time, err error) {
	dateHeader := respHeaders.Get("date")
	if dateHeader == "" {
		err = ErrNoDateHeader
		return
	}

	return time.Parse(time.RFC1123, dateHeader)
}

type realClock struct{}

func (c *realClock) since(d time.Time) time.Duration {
	return time.Since(d)
}

type timer interface {
	since(d time.Time) time.Duration
}

var clock timer = &realClock{}

// getFreshness will return one of fresh/stale/transparent based on the cache-control
// values of the request and the response
//
// fresh indicates the response can be returned
// stale indicates that the response needs validating before it is returned
// transparent indicates the response should not be used to fulfil the request
//
// Because this is only a private cache, 'public' and 'private' in cache-control aren't
// signficant. Similarly, smax-age isn't used.
func getFreshness(respHeaders, reqHeaders http.Header) (freshness int) {
	respCacheControl := parseCacheControl(respHeaders)
	reqCacheControl := parseCacheControl(reqHeaders)
	if _, ok := reqCacheControl["no-cache"]; ok {
		return transparent
	}
	if _, ok := respCacheControl["no-cache"]; ok {
		return stale
	}
	if _, ok := reqCacheControl["only-if-cached"]; ok {
		return fresh
	}

	date, err := Date(respHeaders)
	if err != nil {
		return stale
	}
	currentAge := clock.since(date)

	var lifetime time.Duration
	var zeroDuration time.Duration

	// If a response includes both an Expires header and a max-age directive,
	// the max-age directive overrides the Expires header, even if the Expires header is more restrictive.
	if maxAge, ok := respCacheControl["max-age"]; ok {
		lifetime, err = time.ParseDuration(maxAge + "s")
		if err != nil {
			lifetime = zeroDuration
		}
	} else {
		expiresHeader := respHeaders.Get("Expires")
		if expiresHeader != "" {
			expires, err := time.Parse(time.RFC1123, expiresHeader)
			if err != nil {
				lifetime = zeroDuration
			} else {
				lifetime = expires.Sub(date)
			}
		}
	}

	if maxAge, ok := reqCacheControl["max-age"]; ok {
		// the client is willing to accept a response whose age is no greater than the specified time in seconds
		lifetime, err = time.ParseDuration(maxAge + "s")
		if err != nil {
			lifetime = zeroDuration
		}
	}
	if minfresh, ok := reqCacheControl["min-fresh"]; ok {
		//  the client wants a response that will still be fresh for at least the specified number of seconds.
		minfreshDuration, err := time.ParseDuration(minfresh + "s")
		if err == nil {
			currentAge = time.Duration(currentAge + minfreshDuration)
		}
	}

	if maxstale, ok := reqCacheControl["max-stale"]; ok {
		// Indicates that the client is willing to accept a response that has exceeded its expiration time.
		// If max-stale is assigned a value, then the client is willing to accept a response that has exceeded
		// its expiration time by no more than the specified number of seconds.
		// If no value is assigned to max-stale, then the client is willing to accept a stale response of any age.
		//
		// Responses served only because of a max-stale value are supposed to have a Warning header added to them,
		// but that seems like a  hassle, and is it actually useful? If so, then there needs to be a different
		// return-value available here.
		if maxstale == "" {
			return fresh
		}
		maxstaleDuration, err := time.ParseDuration(maxstale + "s")
		if err == nil {
			currentAge = time.Duration(currentAge - maxstaleDuration)
		}
	}

	if lifetime > currentAge {
		return fresh
	}

	return stale
}

// Returns true if either the request or the response includes the stale-if-error
// cache control extension: https://tools.ietf.org/html/rfc5861
func canStaleOnError(respHeaders, reqHeaders http.Header) bool {
	respCacheControl := parseCacheControl(respHeaders)
	reqCacheControl := parseCacheControl(reqHeaders)

	var err error
	lifetime := time.Duration(-1)

	if staleMaxAge, ok := respCacheControl["stale-if-error"]; ok {
		if staleMaxAge != "" {
			lifetime, err = time.ParseDuration(staleMaxAge + "s")
			if err != nil {
				return false
			}
		} else {
			return true
		}
	}
	if staleMaxAge, ok := reqCacheControl["stale-if-error"]; ok {
		if staleMaxAge != "" {
			lifetime, err = time.ParseDuration(staleMaxAge + "s")
			if err != nil {
				return false
			}
		} else {
			return true
		}
	}

	if lifetime >= 0 {
		date, err := Date(respHeaders)
		if err != nil {
			return false
		}
		currentAge := clock.since(date)
		if lifetime > currentAge {
			return true
		}
	}

	return false
}

func getEndToEndHeaders(respHeaders http.Header) []string {
	// These headers are always hop-by-hop
	hopByHopHeaders := map[string]struct{}{
		"Connection":          {},
		"Keep-Alive":          {},
		"Proxy-Authenticate":  {},
		"Proxy-Authorization": {},
		"Te":                  {},
		"Trailers":            {},
		"Transfer-Encoding":   {},
		"Upgrade":             {},
	}

	for _, extra := range strings.Split(respHeaders.Get("connection"), ",") {
		// any header listed in connection, if present, is also considered hop-by-hop
		if strings.Trim(extra, " ") != "" {
			hopByHopHeaders[http.CanonicalHeaderKey(extra)] = struct{}{}
		}
	}
	endToEndHeaders := []string{}
	for respHeader := range respHeaders {
		if _, ok := hopByHopHeaders[respHeader]; !ok {
			endToEndHeaders = append(endToEndHeaders, respHeader)
		}
	}
	return endToEndHeaders
}

func canStore(reqCacheControl, respCacheControl cacheControl) (canStore bool) {
	if _, ok := respCacheControl["no-store"]; ok {
		return false
	}
	if _, ok := reqCacheControl["no-store"]; ok {
		return false
	}
	return true
}

func newGatewayTimeoutResponse(req *http.Request) *http.Response {
	var braw bytes.Buffer
	braw.WriteString("HTTP/1.1 504 Gateway Timeout\r\n\r\n")
	resp, err := http.ReadResponse(bufio.NewReader(&braw), req)
	if err != nil {
		panic(err)
	}
	return resp
}

// cloneRequest returns a clone of the provided *http.Request.
// The clone is a shallow copy of the struct and its Header map.
// (This function copyright goauth2 authors: https://code.google.com/p/goauth2)
func cloneRequest(r *http.Request) *http.Request {
	// shallow copy of the struct
	r2 := new(http.Request)
	*r2 = *r
	// deep copy of the Header
	r2.Header = make(http.Header)
	for k, s := range r.Header {
		r2.Header[k] = s
	}
	return r2
}

type cacheControl map[string]string

func parseCacheControl(headers http.Header) cacheControl {
	cc := cacheControl{}
	ccHeader := headers.Get("Cache-Control")
	for _, part := range strings.Split(ccHeader, ",") {
		part = strings.Trim(part, " ")
		if part == "" {
			continue
		}
		if strings.ContainsRune(part, '=') {
			keyval := strings.Split(part, "=")
			cc[strings.Trim(keyval[0], " ")] = strings.Trim(keyval[1], ",")
		} else {
			cc[part] = ""
		}
	}
	return cc
}

// headerAllCommaSepValues returns all comma-separated values (each
// with whitespace trimmed) for header name in headers. According to
// Section 4.2 of the HTTP/1.1 spec
// (http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.2),
// values from multiple occurrences of a header should be concatenated, if the
// header's value is a comma-separated list.
func headerAllCommaSepValues(headers http.Header, name string) []string {
	var vals []string
	for _, val := range headers[http.CanonicalHeaderKey(name)] {
		fields := strings.Split(val, ",")
		for i, f := range fields {
			fields[i] = strings.TrimSpace(f)
		}
		vals = append(vals, fields...)
	}
	return vals
}

// candidateReadCloser wraps a cacheable response body. It buffers what the
// caller reads and attempts exactly one cache commit when EOF is reached. The
// commit wins only if the key state observed at departure is still current.
// Closing early, reading a non-EOF error or having the request context
// cancelled all forfeit the commit.
type candidateReadCloser struct {
	// Underlying ReadCloser.
	rc io.ReadCloser
	// attempt whose state rule guards the commit.
	attempt *updateAttempt
	// meta is the independent response metadata captured before the response
	// was handed to the caller.
	meta *http.Response
	// ctxDone, when non-nil, is the cancellation channel of the originating
	// request; a cancelled request never commits.
	ctxDone <-chan struct{}

	mu     sync.Mutex
	buf    bytes.Buffer // buf stores a copy of the content read from rc.
	closed bool
	done   bool // a terminal outcome (commit or forfeiture) has been recorded
}

// Read reads the next len(p) bytes from the underlying body. On EOF the
// buffered response is committed once, subject to the state rule; any other
// error, or a request whose context is already cancelled, forfeits the commit
// without touching the cache.
func (c *candidateReadCloser) Read(p []byte) (n int, err error) {
	n, err = c.rc.Read(p)
	if n > 0 {
		c.buf.Write(p[:n])
	}
	if err != nil {
		c.mu.Lock()
		if !c.done {
			c.done = true
			if err == io.EOF && c.contextAlive() {
				c.commitLocked()
			}
		}
		c.mu.Unlock()
	}
	return n, err
}

// contextAlive reports that the originating request context has not been
// cancelled. Caller must hold c.mu (the read itself is non-blocking).
func (c *candidateReadCloser) contextAlive() bool {
	if c.ctxDone == nil {
		return true
	}
	select {
	case <-c.ctxDone:
		return false
	default:
		return true
	}
}

// commitLocked serializes the buffered candidate and tries to commit it.
// Called with c.mu held, exactly once.
func (c *candidateReadCloser) commitLocked() {
	meta := c.meta
	meta.Body = io.NopCloser(bytes.NewReader(c.buf.Bytes()))
	meta.ContentLength = int64(c.buf.Len())
	respBytes, dumpErr := httputil.DumpResponse(meta, true)
	if dumpErr != nil {
		return
	}
	c.attempt.trySet(respBytes)
}

// Close closes the underlying body. Closing before EOF forfeits any later
// cache commit, so an earlier, slower reader can never restore an entry that
// has since been updated or deleted. Close is safe to call repeatedly; only
// the first call records the outcome or releases the coordination attempt.
func (c *candidateReadCloser) Close() error {
	c.mu.Lock()
	alreadyClosed := c.closed
	if !c.closed {
		c.closed = true
		// Any still-pending commit is now impossible.
		c.done = true
	}
	c.mu.Unlock()
	err := c.rc.Close()
	if !alreadyClosed {
		c.attempt.release()
	}
	return err
}

// releaseReadCloser wraps a body that is returned to the caller without
// caching it, so the coordination attempt ends when the caller closes it.
type releaseReadCloser struct {
	rc      io.ReadCloser
	attempt *updateAttempt
	once    sync.Once
}

func (r *releaseReadCloser) Read(p []byte) (int, error) {
	return r.rc.Read(p)
}

func (r *releaseReadCloser) Close() error {
	err := r.rc.Close()
	r.once.Do(r.attempt.release)
	return err
}

// NewMemoryCacheTransport returns a new Transport using the in-memory cache implementation
func NewMemoryCacheTransport() *Transport {
	c := NewMemoryCache()
	t := NewTransport(c)
	return t
}
