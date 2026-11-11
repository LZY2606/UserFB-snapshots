package httpcache

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net/http"
	"sync"
	"testing"
	"time"
)

// This file drives the real Transport through an http.Client backed by a
// recording RoundTripper. Every ordering is arranged with channel barriers and
// controllable bodies rather than sleeps or random scheduling.

// ---------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------

// recCache is a Cache that records every Set and Delete and keeps immutable
// snapshots of what was stored.
type recCache struct {
	mu      sync.Mutex
	inner   *MemoryCache
	sets    int
	deletes int
	setLog  []string
	delLog  []string
}

func newRecCache() *recCache {
	return &recCache{inner: NewMemoryCache()}
}

func (c *recCache) Get(key string) ([]byte, bool) {
	return c.inner.Get(key)
}

func (c *recCache) Set(key string, b []byte) {
	cp := append([]byte(nil), b...)
	c.mu.Lock()
	c.sets++
	c.setLog = append(c.setLog, key)
	c.mu.Unlock()
	c.inner.Set(key, cp)
}

func (c *recCache) Delete(key string) {
	c.mu.Lock()
	c.deletes++
	c.delLog = append(c.delLog, key)
	c.mu.Unlock()
	c.inner.Delete(key)
}

func (c *recCache) stats() (sets, deletes int) {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sets, c.deletes
}

func (c *recCache) snapshot(key string) ([]byte, bool) {
	b, ok := c.inner.Get(key)
	if !ok {
		return nil, false
	}
	return append([]byte(nil), b...), true
}

// wireCall is a deep-copied record of one downstream request.
type wireCall struct {
	method string
	header http.Header
}

// wire is a recording RoundTripper whose behavior is supplied per test.
type wire struct {
	mu     sync.Mutex
	calls  []wireCall
	handle func(*http.Request) (*http.Response, error)
}

func (w *wire) RoundTrip(r *http.Request) (*http.Response, error) {
	w.mu.Lock()
	w.calls = append(w.calls, wireCall{method: r.Method, header: r.Header.Clone()})
	handle := w.handle
	w.mu.Unlock()
	return handle(r)
}

func (w *wire) callCount() int {
	w.mu.Lock()
	defer w.mu.Unlock()
	return len(w.calls)
}

func (w *wire) call(i int) wireCall {
	w.mu.Lock()
	defer w.mu.Unlock()
	return w.calls[i]
}

// bodyChunk is one scheduled Read outcome for a gateBody.
type bodyChunk struct {
	data []byte
	err  error
}

// gateBody is a response body whose Read outcomes are fed in by the test. It
// records reads and closes so tests can prove nothing is read eagerly.
type gateBody struct {
	chunks chan bodyChunk

	mu      sync.Mutex
	reads   int
	closed  bool
	closeCh chan struct{}
}

func newGateBody() *gateBody {
	return &gateBody{chunks: make(chan bodyChunk, 16), closeCh: make(chan struct{})}
}

func (b *gateBody) Read(p []byte) (int, error) {
	select {
	case c := <-b.chunks:
		b.mu.Lock()
		b.reads++
		b.mu.Unlock()
		n := copy(p, c.data)
		return n, c.err
	case <-b.closeCh:
		return 0, io.ErrClosedPipe
	}
}

func (b *gateBody) Close() error {
	b.mu.Lock()
	if !b.closed {
		b.closed = true
		close(b.closeCh)
	}
	b.mu.Unlock()
	return nil
}

func (b *gateBody) feed(s string)  { b.chunks <- bodyChunk{data: []byte(s)} }
func (b *gateBody) eof()           { b.chunks <- bodyChunk{err: io.EOF} }
func (b *gateBody) fail(err error) { b.chunks <- bodyChunk{err: err} }

func (b *gateBody) readCount() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.reads
}

func (b *gateBody) isClosed() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.closed
}

// fifoBodies hands out queued gate bodies one per downstream call, in FIFO
// order. Because next() blocks until a handler is waiting, the test knows the
// matching request has actually reached the wire before proceeding, which
// keeps concurrent scenarios deterministic without sleeping.
type fifoBodies struct {
	ch chan *gateBody
}

func newFifoBodies() *fifoBodies       { return &fifoBodies{ch: make(chan *gateBody)} }
func (q *fifoBodies) next(b *gateBody) { q.ch <- b }
func (q *fifoBodies) get() *gateBody   { return <-q.ch }

// newEmptyGateBody returns a body whose first Read already reports EOF, for
// 304-style responses that have no entity.
func newEmptyGateBody() *gateBody {
	b := newGateBody()
	b.eof()
	return b
}

// testEnv bundles a Transport, its client, cache and downstream wire.
type testEnv struct {
	t     *testing.T
	tr    *Transport
	c     *recCache
	w     *wire
	cli   *http.Client
	clock *fakeClock
}

const testHTTPDate = "Wed, 01 Jan 2020 00:00:00 GMT"

func newTestEnv(t *testing.T) *testEnv {
	t.Helper()
	c := newRecCache()
	tr := NewTransport(c)
	w := &wire{}
	tr.Transport = w
	fc := &fakeClock{}
	clock = fc
	t.Cleanup(func() { clock = &realClock{} })
	return &testEnv{t: t, tr: tr, c: c, w: w, cli: tr.Client(), clock: fc}
}

func (e *testEnv) setClockElapsed(d time.Duration) { e.clock.elapsed = d }

func (e *testEnv) handle(h func(*http.Request) (*http.Response, error)) { e.w.handle = h }

// resp builds a downstream response with the given status, headers and body.
func (e *testEnv) resp(status int, hdr http.Header, body io.ReadCloser) *http.Response {
	if hdr == nil {
		hdr = http.Header{}
	}
	if _, ok := hdr["Date"]; !ok {
		hdr.Set("Date", testHTTPDate)
	}
	cl := int64(-1)
	if body == nil {
		cl = 0
	}
	return &http.Response{
		StatusCode:    status,
		Status:        http.StatusText(status),
		Proto:         "HTTP/1.1",
		ProtoMajor:    1,
		ProtoMinor:    1,
		Header:        hdr,
		Body:          body,
		ContentLength: cl,
	}
}

func (e *testEnv) textResp(status int, body string, hdr http.Header) *http.Response {
	r := e.resp(status, hdr, io.NopCloser(bytes.NewReader([]byte(body))))
	r.ContentLength = int64(len(body))
	return r
}

func getURL(path string) string { return "http://collector.invalid" + path }

func newGet(t *testing.T, path string) *http.Request {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, getURL(path), nil)
	if err != nil {
		t.Fatal(err)
	}
	return req
}

func readAll(t *testing.T, r io.Reader) string {
	t.Helper()
	b, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("reading body: %v", err)
	}
	return string(b)
}

func mustClose(t *testing.T, c io.Closer) {
	t.Helper()
	if err := c.Close(); err != nil {
		t.Fatalf("close: %v", err)
	}
}

// seed performs a fully-read GET so that path ends up cached with body, and
// resets the cache counters so subsequent test actions can be counted cleanly.
func (e *testEnv) seed(path, body string, hdr http.Header) []byte {
	e.t.Helper()
	e.handle(func(*http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, body, hdr), nil
	})
	resp, err := e.cli.Do(newGet(e.t, path))
	if err != nil {
		e.t.Fatalf("seed request: %v", err)
	}
	if got := readAll(e.t, resp.Body); got != body {
		e.t.Fatalf("seed body = %q, want %q", got, body)
	}
	mustClose(e.t, resp.Body)
	key := cacheKey(newGet(e.t, path))
	stored, ok := e.c.snapshot(key)
	if !ok {
		e.t.Fatal("seed did not populate the cache")
	}
	e.c.mu.Lock()
	e.c.sets = 0
	e.c.deletes = 0
	e.c.setLog = nil
	e.c.delLog = nil
	e.c.mu.Unlock()
	e.w.mu.Lock()
	e.w.calls = nil
	e.w.mu.Unlock()
	return stored
}

func cachedBody(t *testing.T, c Cache, req *http.Request) string {
	t.Helper()
	r, err := CachedResponse(c, req)
	if err != nil || r == nil {
		return ""
	}
	defer r.Body.Close()
	b, err := io.ReadAll(r.Body)
	if err != nil {
		t.Fatalf("reading cached body: %v", err)
	}
	return string(b)
}

// ---------------------------------------------------------------------------
// 1. Caller conditionals stay the caller's business
// ---------------------------------------------------------------------------

// An explicit conditional GET is sent downstream even when a fresh entry
// exists, and an origin 304 comes back verbatim: 304 status, empty body, no
// X-From-Cache, no cache writes or deletes.
func TestCallerConditional304PassedThrough(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"max-age=3600"}, "Etag": {`"old"`}})
	e.setClockElapsed(0) // cached entry is fresh.

	gate := newEmptyGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusNotModified, http.Header{"Etag": {`"caller"`}}, gate), nil
	})

	req := newGet(t, "/items")
	req.Header.Set("If-None-Match", `"caller"`)
	before := req.Header.Clone()
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	if got := resp.Header.Get(XFromCache); got != "" {
		t.Fatalf("explicit 304 carries %s=%q", XFromCache, got)
	}
	if got := readAll(t, resp.Body); got != "" {
		t.Fatalf("304 body = %q, want empty", got)
	}
	mustClose(t, resp.Body)
	if !gate.isClosed() {
		t.Fatal("origin 304 body was not closed")
	}
	sets, deletes := e.c.stats()
	if sets != 0 || deletes != 0 {
		t.Fatalf("cache touched: sets=%d deletes=%d, want 0/0", sets, deletes)
	}
	if e.w.callCount() != 1 {
		t.Fatalf("downstream calls = %d, want 1 (fresh cache must not answer)", e.w.callCount())
	}
	if got := e.w.call(0).header.Get("If-None-Match"); got != `"caller"` {
		t.Fatalf("wire If-None-Match = %q, want caller value", got)
	}
	if !sameHeader(req.Header, before) {
		t.Fatal("the original request Header was modified by RoundTrip")
	}
}

// Caller validators are never replaced or augmented with cached validators.
func TestCallerConditionalValidatorsNotReplaced(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{
		"Cache-Control": {"no-cache"},
		"Etag":          {`"old"`},
		"Last-Modified": {testHTTPDate},
	})

	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusNotModified, nil, newEmptyGateBody()), nil
	})

	req := newGet(t, "/items")
	req.Header.Set("If-None-Match", `"caller"`)
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	mustClose(t, resp.Body)
	wireHeader := e.w.call(0).header
	if got := wireHeader.Values("If-None-Match"); len(got) != 1 || got[0] != `"caller"` {
		t.Fatalf("wire If-None-Match = %v, want only the caller tag", got)
	}
	if got := wireHeader.Get("If-Modified-Since"); got != "" {
		t.Fatalf("wire If-Modified-Since = %q, caller did not supply one", got)
	}
	if got := req.Header.Get("If-Modified-Since"); got != "" {
		t.Fatal("cached Last-Modified was injected into the caller request")
	}

	// Same rule for an If-Modified-Since supplied by the caller.
	req2 := newGet(t, "/items")
	req2.Header.Set("If-Modified-Since", "Wed, 01 Jan 2020 00:00:01 GMT")
	resp2, err := e.cli.Do(req2)
	if err != nil {
		t.Fatal(err)
	}
	mustClose(t, resp2.Body)
	if got := e.w.call(1).header.Values("If-None-Match"); len(got) != 0 {
		t.Fatalf("cached Etag injected: %v", got)
	}
	if got := e.w.call(1).header.Get("If-Modified-Since"); got != "Wed, 01 Jan 2020 00:00:01 GMT" {
		t.Fatalf("wire If-Modified-Since = %q", got)
	}
}

// Weak tags, multiple tags, repeated header lines, both validators at once,
// empty values and non-canonical header names all pass through byte-for-byte.
func TestCallerConditionalHeaderShapesPassThrough(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}, "Etag": {`"old"`}})
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusNotModified, nil, newEmptyGateBody()), nil
	})

	cases := []struct {
		name   string
		header http.Header
	}{
		{
			name:   "weak-etag",
			header: http.Header{"If-None-Match": {`W/"weak"`}},
		},
		{
			name:   "multiple-tags-one-line",
			header: http.Header{"If-None-Match": {`"a", "b", "c"`}},
		},
		{
			name:   "repeated-header-lines",
			header: http.Header{"If-None-Match": {`"a"`, `"b"`}},
		},
		{
			name: "both-validators",
			header: http.Header{
				"If-None-Match":     {`"a"`, `"b"`},
				"If-Modified-Since": {testHTTPDate},
			},
		},
		{
			name:   "empty-value-counts",
			header: http.Header{"If-None-Match": {""}},
		},
		{
			name:   "lowercase-name-empty-value",
			header: http.Header{"if-modified-since": {""}},
		},
	}

	for i, tc := range cases {
		req := newGet(t, "/items")
		req.Header = tc.header.Clone()
		// Keep an independent reference to the backing slice.
		originalINM := append([]string(nil), req.Header["If-None-Match"]...)
		originalIMS := append([]string(nil), req.Header["If-Modified-Since"]...)

		resp, err := e.cli.Do(req)
		if err != nil {
			t.Fatalf("%s: %v", tc.name, err)
		}
		if resp.StatusCode != http.StatusNotModified {
			t.Fatalf("%s: status = %d, want 304", tc.name, resp.StatusCode)
		}
		mustClose(t, resp.Body)

		wireHeader := e.w.call(i).header
		if !sameHeader(wireHeader, tc.header) {
			t.Fatalf("%s: wire header = %v, want %v", tc.name, wireHeader, tc.header)
		}
		if !sameHeader(req.Header, tc.header) {
			t.Fatalf("%s: caller header mutated = %v", tc.name, req.Header)
		}
		if !sameStrings(req.Header["If-None-Match"], originalINM) {
			t.Fatalf("%s: If-None-Match backing slice changed", tc.name)
		}
		if !sameStrings(req.Header["If-Modified-Since"], originalIMS) {
			t.Fatalf("%s: If-Modified-Since backing slice changed", tc.name)
		}
	}
}

// Network errors and 5xx answers to explicit conditionals are reported as-is:
// no stale-if-error fallback, no cache deletion.
func TestCallerConditionalErrorsPassThrough(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{
		"Cache-Control": {"no-cache, stale-if-error"},
		"Etag":          {`"old"`},
	})

	boom := errors.New("downstream unavailable")
	e.handle(func(r *http.Request) (*http.Response, error) {
		return nil, boom
	})
	req := newGet(t, "/items")
	req.Header.Set("If-None-Match", `"caller"`)
	if _, err := e.cli.Do(req); !errors.Is(err, boom) {
		t.Fatalf("err = %v, want the downstream error", err)
	}
	if _, deletes := e.c.stats(); deletes != 0 {
		t.Fatalf("network error caused %d deletes", deletes)
	}
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "OLD" {
		t.Fatalf("cache after error = %q, want OLD", got)
	}

	gate := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusInternalServerError, nil, gate), nil
	})
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", resp.StatusCode)
	}
	mustClose(t, resp.Body)
	if !gate.isClosed() {
		t.Fatal("500 body not closed")
	}
	sets, deletes := e.c.stats()
	if sets != 0 || deletes != 0 {
		t.Fatalf("5xx touched cache: sets=%d deletes=%d", sets, deletes)
	}
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "OLD" {
		t.Fatalf("cache after 5xx = %q, want OLD", got)
	}
}

// An explicit conditional that receives a full 200 still gets cached once the
// body is fully read.
func TestCallerConditional200StillCaches(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}, "Etag": {`"old"`}})
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "NEW", http.Header{"Etag": {`"new"`}}), nil
	})
	req := newGet(t, "/items")
	req.Header.Set("If-None-Match", `"old"`)
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusOK || readAll(t, resp.Body) != "NEW" {
		t.Fatalf("got %d %q, want 200 NEW", resp.StatusCode, readAll(t, resp.Body))
	}
	mustClose(t, resp.Body)
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "NEW" {
		t.Fatalf("cache = %q, want NEW", got)
	}
	sets, _ := e.c.stats()
	if sets != 1 {
		t.Fatalf("sets = %d, want 1", sets)
	}
}

// ---------------------------------------------------------------------------
// 2. GET/HEAD isolation
// ---------------------------------------------------------------------------

func TestCallerConditionalGetHeadIsolation(t *testing.T) {
	e := newTestEnv(t)
	getReq := newGet(t, "/r")
	headReq, err := http.NewRequest(http.MethodHead, getURL("/r"), nil)
	if err != nil {
		t.Fatal(err)
	}
	getKey, headKey := cacheKey(getReq), cacheKey(headReq)
	if getKey == headKey {
		t.Fatal("GET and HEAD must use different cache keys")
	}

	// Seed GET.
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "get-body", http.Header{"Cache-Control": {"max-age=3600"}}), nil
	})
	resp, err := e.cli.Do(getReq)
	if err != nil {
		t.Fatal(err)
	}
	if got := readAll(t, resp.Body); got != "get-body" {
		t.Fatalf("GET seed body = %q, want get-body", got)
	}
	mustClose(t, resp.Body)

	// HEAD goes downstream despite the fresh GET entry, and stores under its
	// own key without touching the GET entity.
	e.handle(func(r *http.Request) (*http.Response, error) {
		// A HEAD response has no entity body; ContentLength stays zero so the
		// response can be serialized for the cache.
		return e.resp(http.StatusOK, http.Header{"Cache-Control": {"max-age=3600"}}, nil), nil
	})
	hresp, err := e.cli.Do(headReq)
	if err != nil {
		t.Fatal(err)
	}
	if hresp.Header.Get(XFromCache) != "" {
		t.Fatal("HEAD must not borrow the GET entity")
	}
	mustClose(t, hresp.Body)
	if e.w.callCount() != 2 {
		t.Fatalf("downstream calls = %d, want 2", e.w.callCount())
	}
	if _, ok := e.c.snapshot(headKey); !ok {
		t.Fatal("HEAD response not stored under HEAD key")
	}
	if got := cachedBody(t, e.c, getReq); got != "get-body" {
		t.Fatalf("GET entity = %q, want get-body", got)
	}

	// Reset counters so the next part only measures the conditional HEAD.
	e.c.mu.Lock()
	e.c.sets, e.c.deletes = 0, 0
	e.c.mu.Unlock()

	// A conditional HEAD is also passed through and never writes.
	gate := newEmptyGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusNotModified, nil, gate), nil
	})
	creq, _ := http.NewRequest(http.MethodHead, getURL("/r"), nil)
	creq.Header.Set("If-None-Match", `"h"`)
	cresp, err := e.cli.Do(creq)
	if err != nil {
		t.Fatal(err)
	}
	if cresp.StatusCode != http.StatusNotModified {
		t.Fatalf("conditional HEAD status = %d, want 304", cresp.StatusCode)
	}
	mustClose(t, cresp.Body)
	if !gate.isClosed() {
		t.Fatal("conditional HEAD 304 body not closed")
	}
	sets, deletes := e.c.stats()
	if sets != 0 || deletes != 0 {
		t.Fatalf("conditional HEAD 304 touched cache: sets=%d deletes=%d, want 0/0", sets, deletes)
	}
}

// ---------------------------------------------------------------------------
// 3. Automatic revalidation
// ---------------------------------------------------------------------------

// A stale entry is revalidated with transport-generated validators; a 304 is
// merged into a 200 built from that exact snapshot, the downstream 304 body is
// closed and the merged response is committed at EOF.
func TestCallerConditionalAuto304MergesAndCommits(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{
		"Cache-Control": {"no-cache"},
		"Etag":          {`"old"`},
	})
	gate := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		if got := r.Header.Get("If-None-Match"); got != `"old"` {
			t.Errorf("auto If-None-Match = %q, want old etag", got)
		}
		return e.resp(http.StatusNotModified,
			http.Header{"Etag": {`"new"`}, "X-Counter": {"7"}}, gate), nil
	})

	resp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("merged status = %d, want 200", resp.StatusCode)
	}
	if resp.Header.Get(XFromCache) != "1" {
		t.Fatalf("merged response missing %s", XFromCache)
	}
	if got := resp.Header.Get("X-Counter"); got != "7" {
		t.Fatalf("merged X-Counter = %q, want 7", got)
	}
	if got := readAll(t, resp.Body); got != "OLD" {
		t.Fatalf("merged body = %q, want OLD snapshot", got)
	}
	mustClose(t, resp.Body)
	if !gate.isClosed() {
		t.Fatal("downstream 304 body was not closed")
	}
	sets, _ := e.c.stats()
	if sets != 1 {
		t.Fatalf("sets = %d, want 1 merged commit", sets)
	}
	stored, _ := e.c.snapshot(cacheKey(newGet(t, "/items")))
	if !bytes.Contains(stored, []byte("X-Counter: 7")) {
		t.Fatalf("stored snapshot missing merged header: %q", stored)
	}
	if !bytes.Contains(stored, []byte("OLD")) {
		t.Fatal("stored snapshot lost the OLD entity")
	}
}

// no-cache requests go downstream without generated validators, so a 304 is
// returned verbatim instead of being merged with a local entity. As before, an
// unmergeable 304 to an ordinary request invalidates the stale entry.
func TestCallerConditionalNoCache304PassesThrough(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}, "Etag": {`"old"`}})
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusNotModified, nil, newEmptyGateBody()), nil
	})
	req := newGet(t, "/items")
	req.Header.Set("Cache-Control", "no-cache")
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	mustClose(t, resp.Body)
	if got := e.w.call(0).header.Get("If-None-Match"); got != "" {
		t.Fatalf("validator generated for no-cache request: %q", got)
	}
	sets, deletes := e.c.stats()
	if sets != 0 {
		t.Fatalf("passthrough 304 wrote cache %d times", sets)
	}
	if deletes != 1 {
		t.Fatalf("deletes = %d, want 1 (stale entry invalidated)", deletes)
	}
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "" {
		t.Fatalf("entry still present: %q", got)
	}
}

// A Vary mismatch means no generated conditional and no merging: the 304 is
// returned as-is.
func TestCallerConditionalVaryMismatch304PassesThrough(t *testing.T) {
	e := newTestEnv(t)
	seedReq := newGet(t, "/v")
	seedReq.Header.Set("Accept", "text/plain")
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "OLD", http.Header{
			"Cache-Control": {"no-cache"},
			"Vary":          {"Accept"},
		}), nil
	})
	sr, err := e.cli.Do(seedReq)
	if err != nil {
		t.Fatal(err)
	}
	mustClose(t, sr.Body)
	e.c.mu.Lock()
	e.c.sets, e.c.deletes = 0, 0
	e.c.mu.Unlock()

	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusNotModified, nil, newEmptyGateBody()), nil
	})
	req := newGet(t, "/v")
	req.Header.Set("Accept", "application/json")
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	mustClose(t, resp.Body)
	if got := e.w.call(0).header.Get("If-None-Match"); got != "" {
		t.Fatalf("validator generated despite Vary mismatch: %q", got)
	}
	sets, deletes := e.c.stats()
	if sets != 0 {
		t.Fatal("304 merged despite Vary mismatch")
	}
	if deletes != 1 {
		t.Fatalf("deletes = %d, want 1 (unmergeable 304 invalidates)", deletes)
	}
}

// With no validator on the stale snapshot there is nothing to send, so a 304
// cannot be tied to any entity and is passed through; the stale entry is still
// invalidated as before.
func TestCallerConditionalNoValidator304PassesThrough(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}})
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusNotModified, nil, newEmptyGateBody()), nil
	})
	resp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	mustClose(t, resp.Body)
	sets, deletes := e.c.stats()
	if sets != 0 {
		t.Fatal("304 cached despite having no validator to match")
	}
	if deletes != 1 {
		t.Fatalf("deletes = %d, want 1 (stale entry invalidated)", deletes)
	}
}

// stale-if-error still works for ordinary requests, and never writes cache.
func TestCallerConditionalStaleIfErrorFallback(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{
		"Cache-Control": {"no-cache, stale-if-error"},
		"Etag":          {`"old"`},
	})
	e.handle(func(r *http.Request) (*http.Response, error) {
		return nil, errors.New("boom")
	})
	req := newGet(t, "/items")
	req.Header.Set("Cache-Control", "stale-if-error")
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatalf("err = %v, want stale fallback", err)
	}
	if resp.StatusCode != http.StatusOK || readAll(t, resp.Body) != "OLD" {
		t.Fatalf("fallback = %d %q", resp.StatusCode, readAll(t, resp.Body))
	}
	mustClose(t, resp.Body)
	sets, deletes := e.c.stats()
	if sets != 0 || deletes != 0 {
		t.Fatalf("fallback touched cache: sets=%d deletes=%d", sets, deletes)
	}
}

// A snapshot replaced while the request is in flight must not be used as a
// stale-if-error fallback, and must not block the new entry.
func TestCallerConditionalStaleIfErrorNotUsedAfterReplacement(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{
		"Cache-Control": {"no-cache, stale-if-error"},
	})
	proceed := make(chan struct{})
	arrived := make(chan struct{})
	e.handle(func(r *http.Request) (*http.Response, error) {
		close(arrived)
		<-proceed
		return e.resp(http.StatusInternalServerError, nil, newGateBody()), nil
	})

	got := make(chan *http.Response, 1)
	go func() {
		resp, err := e.cli.Do(newGet(t, "/items"))
		if err != nil {
			t.Error(err)
			return
		}
		got <- resp
	}()
	<-arrived // A has reached the wire before B departs.

	// A second request fully writes v2 while the first is still in flight.
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "NEW", http.Header{"Cache-Control": {"max-age=3600"}}), nil
	})
	breq := newGet(t, "/items")
	bresp, err := e.cli.Do(breq)
	if err != nil {
		t.Fatal(err)
	}
	if readAll(t, bresp.Body) != "NEW" {
		t.Fatal("B did not read NEW")
	}
	mustClose(t, bresp.Body)

	close(proceed)
	resp := <-got
	if resp.StatusCode != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500 (stale snapshot must not be reused)", resp.StatusCode)
	}
	mustClose(t, resp.Body)
	if gotBody := cachedBody(t, e.c, newGet(t, "/items")); gotBody != "NEW" {
		t.Fatalf("cache = %q, want NEW", gotBody)
	}
}

// A revalidation 304 that loses the state race still serves the caller the
// snapshot it asked for, but its merged headers cannot reach the new entity.
func TestCallerConditionalAuto304LosesRace(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{
		"Cache-Control": {"no-cache"},
		"Etag":          {`"old"`},
	})
	proceed := make(chan struct{})
	arrived := make(chan struct{})
	gateA := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		close(arrived)
		<-proceed // A waits at the origin.
		return e.resp(http.StatusNotModified,
			http.Header{"Etag": {`"merged"`}, "X-Counter": {"99"}}, gateA), nil
	})

	type result struct {
		resp *http.Response
		err  error
	}
	done := make(chan result, 1)
	go func() {
		resp, err := e.cli.Do(newGet(t, "/items"))
		done <- result{resp, err}
	}()
	<-arrived // A has reached the wire before B departs.

	// B writes v2 while A waits.
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "NEW", http.Header{"Etag": {`"new"`}}), nil
	})
	bresp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if readAll(t, bresp.Body) != "NEW" {
		t.Fatal("B body mismatch")
	}
	mustClose(t, bresp.Body)

	close(proceed)
	res := <-done
	if res.err != nil {
		t.Fatal(res.err)
	}
	if res.resp.StatusCode != http.StatusOK || readAll(t, res.resp.Body) != "OLD" {
		t.Fatalf("A should still receive its OLD snapshot, got %d", res.resp.StatusCode)
	}
	mustClose(t, res.resp.Body)
	if !gateA.isClosed() {
		t.Fatal("A's downstream 304 body not closed")
	}
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "NEW" {
		t.Fatalf("cache = %q, want NEW (A's merged 304 lost the race)", got)
	}
	stored, _ := e.c.snapshot(cacheKey(newGet(t, "/items")))
	if bytes.Contains(stored, []byte("X-Counter: 99")) {
		t.Fatal("lost 304's headers leaked into the winning entity")
	}
}

// ---------------------------------------------------------------------------
// 4. Commit ordering and the state generation rule
// ---------------------------------------------------------------------------

// Two requests depart from the same state. The first valid commit wins; the
// loser still hands its own full response to its caller but cannot write.
func TestCallerConditionalFirstEOFWins(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "v1", http.Header{"Cache-Control": {"no-cache"}})

	gateA, gateB := newGateBody(), newGateBody()
	queue := newFifoBodies()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, queue.get()), nil
	})

	type outcome struct {
		body string
		err  error
	}
	out := make(chan outcome, 2)
	run := func() {
		resp, err := e.cli.Do(newGet(t, "/items"))
		if err != nil {
			out <- outcome{err: err}
			return
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		out <- outcome{body: string(b)}
	}
	go run()
	go run()

	// Hand bodies to the wire in FIFO: A first, then B, each blocking until
	// its request actually arrives.
	queue.next(gateA)
	queue.next(gateB)

	// B (v2) reaches EOF and fully completes first. A's gate is still empty,
	// so its reader is blocked and cannot commit yet.
	gateB.feed("v2-data")
	gateB.eof()
	first := <-out
	if first.err != nil || first.body != "v2-data" {
		t.Fatalf("first completion = %+v, want v2-data", first)
	}
	if body := cachedBody(t, e.c, newGet(t, "/items")); body != "v2-data" {
		t.Fatalf("cache after B = %q, want v2-data", body)
	}

	// Only now does A (v3) reach EOF; it must lose despite being the older
	// request.
	gateA.feed("v3-data")
	gateA.eof()
	second := <-out
	if second.err != nil || second.body != "v3-data" {
		t.Fatalf("second completion = %+v, want v3-data (still delivered in full)", second)
	}
	if body := cachedBody(t, e.c, newGet(t, "/items")); body != "v2-data" {
		t.Fatalf("cache = %q, want v2-data (first valid commit wins)", body)
	}
	sets, _ := e.c.stats()
	if sets != 1 {
		t.Fatalf("Cache.Set calls = %d, want 1", sets)
	}
}

// If the earlier request closes before EOF it forfeits, so the later valid
// read wins.
func TestCallerConditionalEarlyCloseForfeits(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "v1", http.Header{"Cache-Control": {"no-cache"}})

	gateA, gateB := newGateBody(), newGateBody()
	arrived := make(chan struct{})
	var callNum int
	var mu sync.Mutex
	e.handle(func(r *http.Request) (*http.Response, error) {
		mu.Lock()
		callNum++
		n := callNum
		mu.Unlock()
		if n == 1 {
			close(arrived)
			return e.resp(http.StatusOK, nil, gateA), nil
		}
		return e.resp(http.StatusOK, nil, gateB), nil
	})

	closed := make(chan struct{})
	go func() {
		resp, err := e.cli.Do(newGet(t, "/items")) // A departs first.
		if err != nil {
			t.Error(err)
			return
		}
		gateA.feed("partial-A") // caller read something, then gave up.
		buf := make([]byte, 16)
		if n, _ := resp.Body.Read(buf); n != 9 {
			t.Errorf("A read %d, want 9", n)
		}
		resp.Body.Close()
		close(closed)
	}()

	<-arrived // A has reached the wire before B departs.
	bresp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	<-closed // A has forfeited via early close.
	gateB.feed("v3-data")
	gateB.eof()
	if got := readAll(t, bresp.Body); got != "v3-data" {
		t.Fatalf("B body = %q, want v3-data", got)
	}
	mustClose(t, bresp.Body)

	if body := cachedBody(t, e.c, newGet(t, "/items")); body != "v3-data" {
		t.Fatalf("cache = %q, want v3-data (A's early close forfeited)", body)
	}
	sets, _ := e.c.stats()
	if sets != 1 {
		t.Fatalf("Cache.Set calls = %d, want 1", sets)
	}
}

// A non-EOF read error forfeits the commit and leaves the prior entry intact.
func TestCallerConditionalBodyErrorNoCommit(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}})
	gate := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, gate), nil
	})
	resp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	gate.feed("part")
	buf := make([]byte, 16)
	if n, _ := resp.Body.Read(buf); n != 4 {
		t.Fatalf("read %d bytes, want 4", n)
	}
	readErr := errors.New("connection broke")
	gate.fail(readErr)
	if _, err := resp.Body.Read(buf); !errors.Is(err, readErr) {
		t.Fatalf("read err = %v, want %v", err, readErr)
	}
	mustClose(t, resp.Body)
	sets, _ := e.c.stats()
	if sets != 0 {
		t.Fatalf("sets = %d, want 0 after body error", sets)
	}
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "OLD" {
		t.Fatalf("cache = %q, want OLD preserved", got)
	}
}

// A request whose context is already cancelled when EOF arrives must not
// commit.
func TestCallerConditionalCancelledContextNoCommit(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}})
	gate := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, gate), nil
	})
	ctx, cancel := context.WithCancel(context.Background())
	req := newGet(t, "/items")
	req = req.WithContext(ctx)
	resp, err := e.cli.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	gate.feed("part")
	buf := make([]byte, 16)
	if n, _ := resp.Body.Read(buf); n != 4 {
		t.Fatalf("read %d, want 4", n)
	}
	cancel()
	gate.eof()
	if _, err := resp.Body.Read(buf); err != io.EOF {
		t.Fatalf("err = %v, want EOF", err)
	}
	mustClose(t, resp.Body)
	if sets, _ := e.c.stats(); sets != 0 {
		t.Fatalf("sets = %d, want 0 after cancellation", sets)
	}
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "OLD" {
		t.Fatalf("cache = %q, want OLD", got)
	}
}

// no-store invalidates at response-header time, before the body is read, and a
// delete of an absent key still counts as a new state.
func TestCallerConditionalNoStoreDeletesAtHeaders(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}})
	gate := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, http.Header{"Cache-Control": {"no-store"}}, gate), nil
	})
	resp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if _, ok := e.c.snapshot(cacheKey(newGet(t, "/items"))); ok {
		t.Fatal("key still present before no-store body was read")
	}
	if _, deletes := e.c.stats(); deletes != 1 {
		t.Fatalf("deletes = %d, want 1 at header time", deletes)
	}
	gate.feed("ignored")
	gate.eof()
	io.Copy(io.Discard, resp.Body)
	mustClose(t, resp.Body)
	if sets, _ := e.c.stats(); sets != 0 {
		t.Fatalf("sets = %d, no-store body must never be stored", sets)
	}

	// A second no-store against the missing key still advances the state.
	gate2 := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, http.Header{"Cache-Control": {"no-store"}}, gate2), nil
	})
	r2, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	mustClose(t, r2.Body)
	if _, deletes := e.c.stats(); deletes != 2 {
		t.Fatalf("deletes = %d, deleting a missing key must still create state", deletes)
	}
}

// A no-store response arriving first blocks an older, not-yet-read response
// from resurrecting the entry.
func TestCallerConditionalNoStoreRaceBlocksOlderWriter(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "v1", http.Header{"Cache-Control": {"no-cache"}})
	gateA := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, gateA), nil // A: storable, held open
	})
	ares, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}

	// B's no-store response completes (header-time delete) while A is open.
	gateB := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, http.Header{"Cache-Control": {"no-store"}}, gateB), nil
	})
	bres, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	mustClose(t, bres.Body)
	if _, ok := e.c.snapshot(cacheKey(newGet(t, "/items"))); ok {
		t.Fatal("no-store did not remove the entry")
	}

	// A finishing afterwards cannot resurrect it.
	gateA.feed("v3-data")
	gateA.eof()
	io.Copy(io.Discard, ares.Body)
	mustClose(t, ares.Body)
	if _, ok := e.c.snapshot(cacheKey(newGet(t, "/items"))); ok {
		t.Fatal("stale candidate resurrected a deleted entry")
	}
	sets, deletes := e.c.stats()
	if sets != 0 || deletes != 1 {
		t.Fatalf("sets=%d deletes=%d, want 0/1", sets, deletes)
	}
}

// The reverse ordering: a no-store response that arrives after a newer valid
// write has lost its eligibility and cannot delete the new entry.
func TestCallerConditionalLateNoStoreCannotDeleteNewEntry(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "v1", http.Header{"Cache-Control": {"no-cache"}})

	proceed := make(chan struct{})
	arrived := make(chan struct{})
	gateA := newEmptyGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		close(arrived)
		<-proceed // A (no-store) waits at the origin.
		return e.resp(http.StatusOK, http.Header{"Cache-Control": {"no-store"}}, gateA), nil
	})
	adone := make(chan struct{})
	go func() {
		resp, err := e.cli.Do(newGet(t, "/items"))
		if err != nil {
			t.Error(err)
			return
		}
		io.Copy(io.Discard, resp.Body)
		resp.Body.Close()
		close(adone)
	}()
	<-arrived // A has reached the wire before B departs.

	// B fully writes v2 first.
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "v2-data", nil), nil
	})
	bresp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if readAll(t, bresp.Body) != "v2-data" {
		t.Fatal("B body mismatch")
	}
	mustClose(t, bresp.Body)

	close(proceed)
	<-adone
	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "v2-data" {
		t.Fatalf("cache = %q, stale no-store must not delete v2", got)
	}
	sets, deletes := e.c.stats()
	if sets != 1 || deletes != 0 {
		t.Fatalf("sets=%d deletes=%d, want 1/0", sets, deletes)
	}
}

// Delete followed by a re-write of byte-identical content is still a new
// state: an older in-flight reader loses even though the serialized bytes are
// exactly equal.
func TestCallerConditionalSameBytesStillNewGeneration(t *testing.T) {
	e := newTestEnv(t)
	hdr := http.Header{"Cache-Control": {"no-cache"}, "Etag": {`"same"`}}
	v1 := e.seed("/items", "SAME", hdr)

	gateA := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, gateA), nil
	})
	ares, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}

	// B: no-store deletes.
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, http.Header{"Cache-Control": {"no-store"}}, newGateBody()), nil
	})
	bres, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	mustClose(t, bres.Body)

	// C: writes content that serializes to the exact v1 bytes.
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "SAME", hdr.Clone()), nil
	})
	cres, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if readAll(t, cres.Body) != "SAME" {
		t.Fatal("C body mismatch")
	}
	mustClose(t, cres.Body)
	stored, ok := e.c.snapshot(cacheKey(newGet(t, "/items")))
	if !ok || !bytes.Equal(stored, v1) {
		t.Fatal("C should have stored byte-identical content")
	}

	// A now finishes: identical bytes do not make its snapshot current again.
	gateA.feed("SAME")
	gateA.eof()
	io.Copy(io.Discard, ares.Body)
	mustClose(t, ares.Body)
	sets, deletes := e.c.stats()
	if sets != 1 || deletes != 1 {
		t.Fatalf("sets=%d deletes=%d, want C's 1 set and B's 1 delete", sets, deletes)
	}
	final, ok := e.c.snapshot(cacheKey(newGet(t, "/items")))
	if !ok || !bytes.Equal(final, v1) {
		t.Fatal("final cache content changed")
	}
}

// The generation rule applies when no entry existed at departure too.
func TestCallerConditionalEmptyCacheRace(t *testing.T) {
	e := newTestEnv(t)
	gateA, gateB := newGateBody(), newGateBody()
	queue := newFifoBodies()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, queue.get()), nil
	})
	out := make(chan string, 2)
	run := func() {
		resp, err := e.cli.Do(newGet(t, "/empty"))
		if err != nil {
			t.Error(err)
			return
		}
		b, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		out <- string(b)
	}
	go run()
	go run()
	queue.next(gateA)
	queue.next(gateB)

	// B completes first while A's gate is still empty, so A's reader blocks.
	gateB.feed("first-wins")
	gateB.eof()
	first := <-out
	if first != "first-wins" {
		t.Fatalf("first completion = %q, want first-wins", first)
	}
	if got := cachedBody(t, e.c, newGet(t, "/empty")); got != "first-wins" {
		t.Fatalf("cache after first = %q, want first-wins", got)
	}

	// A finishes second from the same empty-cache departure state and loses.
	gateA.feed("second-loses")
	gateA.eof()
	second := <-out
	if second != "second-loses" {
		t.Fatalf("second completion = %q, want second-loses (still fully delivered)", second)
	}
	if got := cachedBody(t, e.c, newGet(t, "/empty")); got != "first-wins" {
		t.Fatalf("cache = %q, want first-wins", got)
	}
	if sets, _ := e.c.stats(); sets != 1 {
		t.Fatalf("sets = %d, want 1", sets)
	}
}

// Reaching EOF twice and closing repeatedly must not repeat Set/Delete or
// re-acquire eligibility after a lost race.
func TestCallerConditionalIdempotentEOFAndClose(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "v1", http.Header{"Cache-Control": {"no-cache"}})

	gateA, gateB := newGateBody(), newGateBody()
	queue := newFifoBodies()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, queue.get()), nil
	})

	go func() { queue.next(gateA) }()
	ares, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	go func() { queue.next(gateB) }()
	bresp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	gateB.feed("B")
	gateB.eof()
	io.Copy(io.Discard, bresp.Body)
	bresp.Body.Close()
	bresp.Body.Close() // repeated close is a no-op.

	gateA.feed("A")
	gateA.eof()
	gateA.eof() // a second EOF must not commit again.
	buf := make([]byte, 16)
	for {
		_, err := ares.Body.Read(buf)
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
	}
	if _, err := ares.Body.Read(buf); err != io.EOF {
		t.Fatalf("second EOF read err = %v", err)
	}
	ares.Body.Close()
	ares.Body.Close()

	if got := cachedBody(t, e.c, newGet(t, "/items")); got != "B" {
		t.Fatalf("cache = %q, want B", got)
	}
	sets, deletes := e.c.stats()
	if sets != 1 || deletes != 0 {
		t.Fatalf("sets=%d deletes=%d, want 1/0", sets, deletes)
	}
}

// ---------------------------------------------------------------------------
// 5. Freshness, isolation, metadata safety and resource reclamation
// ---------------------------------------------------------------------------

func TestCallerConditionalFreshAndStaleBehavior(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{
		"Cache-Control": {"max-age=3600"},
		"Etag":          {`"old"`},
	})

	// Fresh: served from cache, no downstream call.
	e.setClockElapsed(10 * time.Second)
	resp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if resp.Header.Get(XFromCache) != "1" || readAll(t, resp.Body) != "OLD" {
		t.Fatal("fresh entry not served from cache")
	}
	mustClose(t, resp.Body)
	if e.w.callCount() != 0 {
		t.Fatalf("fresh hit made %d downstream calls", e.w.callCount())
	}

	// Stale: revalidated with the cached validator.
	e.setClockElapsed(4000 * time.Second)
	e.handle(func(r *http.Request) (*http.Response, error) {
		if got := r.Header.Get("If-None-Match"); got != `"old"` {
			t.Errorf("validator = %q, want old etag", got)
		}
		return e.textResp(http.StatusOK, "NEW", nil), nil
	})
	resp, err = e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if readAll(t, resp.Body) != "NEW" {
		t.Fatal("stale revalidation did not return NEW")
	}
	mustClose(t, resp.Body)
	if e.w.callCount() != 1 {
		t.Fatalf("downstream calls = %d, want 1", e.w.callCount())
	}
}

// The Transport never reads ahead: nothing is consumed from the downstream
// body before the caller asks for it, and no background reader drains it.
func TestCallerConditionalNoEagerBodyRead(t *testing.T) {
	e := newTestEnv(t)
	gate := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, gate), nil
	})
	resp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	if gate.readCount() != 0 {
		t.Fatalf("Transport read %d chunks before the caller", gate.readCount())
	}
	gate.feed("data")
	buf := make([]byte, 4)
	n, err := resp.Body.Read(buf)
	if err != nil || n != 4 || string(buf) != "data" {
		t.Fatalf("first read = %d %q %v", n, buf[:n], err)
	}
	if gate.readCount() != 1 {
		t.Fatalf("downstream reads = %d, want 1", gate.readCount())
	}
	gate.eof()
	if _, err := io.Copy(io.Discard, resp.Body); err != nil {
		t.Fatal(err)
	}
	mustClose(t, resp.Body)
}

// Header mutations after RoundTrip returns cannot flow into cached metadata,
// and two in-flight responses own independent header maps and slices.
func TestCallerConditionalReturnedHeaderMutationIsolated(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}})

	gateA, gateB := newGateBody(), newGateBody()
	resps := make(chan *http.Response, 2)
	resps <- e.resp(http.StatusOK, http.Header{"X-Id": {"A"}}, gateA)
	resps <- e.resp(http.StatusOK, http.Header{"X-Id": {"B"}}, gateB)
	e.handle(func(r *http.Request) (*http.Response, error) {
		return <-resps, nil
	})
	ra, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	rb, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	ra.Header.Set("X-Mutated", "ra")
	rb.Header.Set("X-Mutated", "rb")
	if ra.Header.Get("X-Id") != "A" || rb.Header.Get("X-Id") != "B" {
		t.Fatal("response headers bled into each other")
	}
	gateB.feed("B-body")
	gateB.eof()
	io.Copy(io.Discard, rb.Body)
	mustClose(t, rb.Body)
	gateA.feed("A-body")
	gateA.eof()
	io.Copy(io.Discard, ra.Body)
	mustClose(t, ra.Body)

	// B won; its stored metadata must contain none of A's or the callers'
	// mutations.
	stored, ok := e.c.snapshot(cacheKey(newGet(t, "/items")))
	if !ok {
		t.Fatal("nothing cached")
	}
	if bytes.Contains(stored, []byte("X-Mutated")) {
		t.Fatal("caller header mutation leaked into cached metadata")
	}
	if !bytes.Contains(stored, []byte("X-Id: B")) {
		t.Fatal("winning response header missing from cache")
	}
	if bytes.Contains(stored, []byte("X-Id: A")) {
		t.Fatal("losing response metadata leaked into cache")
	}
}

// Per-key coordination state exists only while a request (including its body)
// is in flight, and is reclaimed afterwards.
func TestCallerConditionalCoordinationReclaimed(t *testing.T) {
	e := newTestEnv(t)
	key := getURL("/items")
	if _, ok := e.tr.coords[key]; ok {
		t.Fatal("coordinator exists before any request")
	}
	gate := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.resp(http.StatusOK, nil, gate), nil
	})
	resp, err := e.cli.Do(newGet(t, "/items"))
	if err != nil {
		t.Fatal(err)
	}
	kc, ok := e.tr.coords[key]
	if !ok {
		t.Fatal("coordinator missing while body is in flight")
	}
	if kc.inFlight != 1 {
		t.Fatalf("inFlight = %d, want 1", kc.inFlight)
	}
	gate.feed("done")
	gate.eof()
	io.Copy(io.Discard, resp.Body)
	mustClose(t, resp.Body)
	if _, ok := e.tr.coords[key]; ok {
		t.Fatal("coordinator retained after the request ended")
	}
}

// An open body on one key must not block traffic or coordination on another
// key; the transport never serializes the whole RoundTrip lifecycle.
func TestCallerConditionalOtherKeyNotBlocked(t *testing.T) {
	e := newTestEnv(t)
	gate1 := newGateBody()
	e.handle(func(r *http.Request) (*http.Response, error) {
		if r.URL.Path == "/k1" {
			return e.resp(http.StatusOK, nil, gate1), nil
		}
		return e.textResp(http.StatusOK, "k2-body", http.Header{"Cache-Control": {"max-age=3600"}}), nil
	})
	r1, err := e.cli.Do(newGet(t, "/k1"))
	if err != nil {
		t.Fatal(err)
	}
	if len(e.tr.coords) != 1 {
		t.Fatalf("coordinators = %d, want 1 for k1", len(e.tr.coords))
	}
	r2, err := e.cli.Do(newGet(t, "/k2"))
	if err != nil {
		t.Fatal(err)
	}
	if readAll(t, r2.Body) != "k2-body" {
		t.Fatal("k2 was blocked by the open k1 body")
	}
	// k2 is fully read (and committed) but not yet closed, so both keys have
	// live coordinators.
	if len(e.tr.coords) != 2 {
		t.Fatalf("coordinators = %d, want independent entries for k1 and k2", len(e.tr.coords))
	}
	mustClose(t, r2.Body)
	if _, ok := e.c.snapshot(cacheKey(newGet(t, "/k2"))); !ok {
		t.Fatal("k2 was not cached while k1 stayed open")
	}
	// Closing k2 reclaims only its coordinator; k1 stays.
	if len(e.tr.coords) != 1 {
		t.Fatalf("coordinators = %d, want only k1 retained after k2 closed", len(e.tr.coords))
	}
	gate1.feed("k1-body")
	gate1.eof()
	io.Copy(io.Discard, r1.Body)
	mustClose(t, r1.Body)
	if len(e.tr.coords) != 0 {
		t.Fatalf("coordinators = %v, want all reclaimed", e.tr.coords)
	}
}

// The auto-validation path clones the request, so the caller's request never
// gains validator headers.
func TestCallerConditionalAutoValidationDoesNotMutateRequest(t *testing.T) {
	e := newTestEnv(t)
	e.seed("/items", "OLD", http.Header{"Cache-Control": {"no-cache"}, "Etag": {`"old"`}})
	e.handle(func(r *http.Request) (*http.Response, error) {
		return e.textResp(http.StatusOK, "NEW", nil), nil
	})
	req := newGet(t, "/items")
	if _, err := e.cli.Do(req); err != nil {
		t.Fatal(err)
	}
	if got := req.Header.Get("If-None-Match"); got != "" {
		t.Fatalf("caller request mutated with If-None-Match %q", got)
	}
}

// ---------------------------------------------------------------------------
// Small assertion helpers
// ---------------------------------------------------------------------------

func sameHeader(a, b http.Header) bool {
	if len(a) != len(b) {
		return false
	}
	for k, va := range a {
		vb, ok := b[k]
		if !ok || !sameStrings(va, vb) {
			return false
		}
	}
	return true
}

func sameStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
