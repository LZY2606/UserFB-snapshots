package httpcache

import (
	"bufio"
	"bytes"
	"errors"
	"io"
	"net/http"
	"reflect"
	"testing"
	"time"
)

// wireCall records one request handed to the scripted downstream transport,
// together with a deep copy of its headers as they were on the wire.
type wireCall struct {
	req     *http.Request
	headers http.Header
}

// scriptedTransport is a recording http.RoundTripper: it remembers every
// request the caching Transport sends downstream and replies through the
// configured responder. It never opens a socket, so the tests drive the real
// Transport (through an http.Client) without any network dependency.
type scriptedTransport struct {
	calls   []wireCall
	respond func(call int, req *http.Request) (*http.Response, error)
}

func (s *scriptedTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	s.calls = append(s.calls, wireCall{req: req, headers: copyHeader(req.Header)})
	if s.respond == nil {
		return nil, errors.New("scriptedTransport: no responder configured")
	}
	return s.respond(len(s.calls), req)
}

func copyHeader(h http.Header) http.Header {
	cp := make(http.Header, len(h))
	for key, values := range h {
		cp[key] = append([]string(nil), values...)
	}
	return cp
}

// newScriptedResponse builds a minimal but well-formed response with a body.
func newScriptedResponse(status int, body string, header http.Header) *http.Response {
	if header == nil {
		header = http.Header{}
	}
	return &http.Response{
		Status:        http.StatusText(status),
		StatusCode:    status,
		Proto:         "HTTP/1.1",
		ProtoMajor:    1,
		ProtoMinor:    1,
		Header:        header,
		Body:          io.NopCloser(bytes.NewReader([]byte(body))),
		ContentLength: int64(len(body)),
	}
}

// newTestClient wires an http.Client to a caching Transport backed by a
// recording in-memory transport.
func newTestClient(wire *scriptedTransport) (*http.Client, *MemoryCache) {
	cache := NewMemoryCache()
	transport := NewTransport(cache)
	transport.Transport = wire
	return &http.Client{Transport: transport}, cache
}

const callerConditionalURL = "http://collector.invalid/items"

// seedCallerConditionalCache performs a plain GET whose 200 response is fully
// read, so it ends up stored in the cache.
func seedCallerConditionalCache(t *testing.T, client *http.Client, header http.Header, body string) {
	t.Helper()
	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("seed request failed: %v", err)
	}
	got, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("reading seed body: %v", err)
	}
	if err := resp.Body.Close(); err != nil {
		t.Fatalf("closing seed body: %v", err)
	}
	if string(got) != body {
		t.Fatalf("seed body = %q, want %q", got, body)
	}
}

// readCachedEntry parses the raw bytes stored for key, returning nil when the
// key is absent.
func readCachedEntry(t *testing.T, cache *MemoryCache, key string) *http.Response {
	t.Helper()
	raw, ok := cache.Get(key)
	if !ok {
		return nil
	}
	resp, err := http.ReadResponse(bufio.NewReader(bytes.NewReader(raw)), nil)
	if err != nil {
		t.Fatalf("parsing cached entry: %v", err)
	}
	return resp
}

func readBody(t *testing.T, resp *http.Response) string {
	t.Helper()
	body, err := io.ReadAll(resp.Body)
	if err != nil {
		t.Fatalf("reading response body: %v", err)
	}
	return string(body)
}

func freshSeedHeader() http.Header {
	return http.Header{
		"Date":          {time.Now().Format(time.RFC1123)},
		"Cache-Control": {"max-age=3600"},
		"Etag":          {`"old"`},
	}
}

func staleSeedHeader(extra string) http.Header {
	cc := "no-cache"
	if extra != "" {
		cc = cc + ", " + extra
	}
	return http.Header{
		"Date":          {time.Now().Format(time.RFC1123)},
		"Cache-Control": {cc},
		"Etag":          {`"old"`},
		"Last-Modified": {"Fri, 14 Dec 2010 01:01:50 GMT"},
	}
}

// A fresh cache entry must not short-circuit a caller-owned conditional
// request: the caller's If-None-Match and no-cache still go downstream and
// the origin's 304 comes back untouched instead of becoming the cached 200.
func TestCallerConditionalFresh304(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", freshSeedHeader()), nil
		}
		return newScriptedResponse(http.StatusNotModified, "", http.Header{"Etag": {`"caller"`}}), nil
	}

	seedCallerConditionalCache(t, client, freshSeedHeader(), "OLD")

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"caller"`)
	req.Header.Set("Cache-Control", "no-cache")
	before := copyHeader(req.Header)

	resp, err := client.Do(req)
	if err != nil {
		t.Fatalf("conditional request failed: %v", err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	if body := readBody(t, resp); body != "" {
		t.Fatalf("304 body = %q, want empty", body)
	}
	if err := resp.Body.Close(); err != nil {
		t.Fatal(err)
	}
	if got := resp.Header.Get(XFromCache); got != "" {
		t.Fatalf("X-From-Cache = %q, want absent", got)
	}

	if len(wire.calls) != 2 {
		t.Fatalf("downstream calls = %d, want 2 (fresh cache must not answer)", len(wire.calls))
	}
	seen := wire.calls[1].headers
	if got := seen.Get("If-None-Match"); got != `"caller"` {
		t.Fatalf("downstream If-None-Match = %q, want %q", got, `"caller"`)
	}
	if got := seen.Get("Cache-Control"); got != "no-cache" {
		t.Fatalf("downstream Cache-Control = %q, want no-cache", got)
	}
	if !reflect.DeepEqual(req.Header, before) {
		t.Fatalf("caller request headers mutated:\n before %v\n after  %v", before, req.Header)
	}

	cached := readCachedEntry(t, cache, callerConditionalURL)
	if cached == nil {
		t.Fatal("cached entry was deleted by the explicit 304")
	}
	if cached.StatusCode != http.StatusOK {
		t.Fatalf("cached status = %d, want 200", cached.StatusCode)
	}
	if got := readBody(t, cached); got != "OLD" {
		t.Fatalf("cached body = %q, want OLD", got)
	}
	if got := cached.Header.Get("Etag"); got != `"old"` {
		t.Fatalf("cached etag = %q, want %q", got, `"old"`)
	}
}

// With a stale entry the Transport must not append its own validators to a
// caller-owned conditional request, and must not merge the 304 into the
// cached entity.
func TestCallerConditionalStale304IfNoneMatch(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("")), nil
		}
		return newScriptedResponse(http.StatusNotModified, "", http.Header{"Etag": {`"caller"`}}), nil
	}

	seedCallerConditionalCache(t, client, staleSeedHeader(""), "OLD")

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"caller"`)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	if body := readBody(t, resp); body != "" {
		t.Fatalf("304 body = %q, want empty", body)
	}
	resp.Body.Close()
	if got := resp.Header.Get(XFromCache); got != "" {
		t.Fatalf("X-From-Cache = %q, want absent", got)
	}

	seen := wire.calls[1].headers
	if got := seen.Get("If-None-Match"); got != `"caller"` {
		t.Fatalf("downstream If-None-Match = %q, want %q", got, `"caller"`)
	}
	if got, ok := seen["If-None-Match"]; len(got) != 1 || !ok {
		t.Fatalf("downstream If-None-Match values = %v, want single caller value", got)
	}
	// The cached Last-Modified must not be turned into an implicit validator.
	if got := seen.Get("If-Modified-Since"); got != "" {
		t.Fatalf("downstream If-Modified-Since = %q, want absent", got)
	}

	cached := readCachedEntry(t, cache, callerConditionalURL)
	if cached == nil || cached.StatusCode != http.StatusOK {
		t.Fatalf("cached entry changed after explicit 304: %+v", cached)
	}
	if got := readBody(t, cached); got != "OLD" {
		t.Fatalf("cached body = %q, want OLD", got)
	}
}

// If-Modified-Since alone is the caller's condition as well: it is forwarded
// verbatim and no cached ETag validator is attached.
func TestCallerConditionalStale304IfModifiedSince(t *testing.T) {
	wire := &scriptedTransport{}
	client, _ := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("")), nil
		}
		return newScriptedResponse(http.StatusNotModified, "", nil), nil
	}

	seedCallerConditionalCache(t, client, staleSeedHeader(""), "OLD")

	const callerDate = "Wed, 01 Jan 2020 00:00:00 GMT"
	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-Modified-Since", callerDate)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	if body := readBody(t, resp); body != "" {
		t.Fatalf("304 body = %q, want empty", body)
	}
	resp.Body.Close()

	seen := wire.calls[1].headers
	if got := seen.Get("If-Modified-Since"); got != callerDate {
		t.Fatalf("downstream If-Modified-Since = %q, want %q", got, callerDate)
	}
	if got := seen.Get("If-None-Match"); got != "" {
		t.Fatalf("downstream If-None-Match = %q, want absent", got)
	}
}

// When both validators are present they both go through unchanged, and the
// cached validators are neither added nor substituted.
func TestCallerConditionalStale304BothValidators(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("")), nil
		}
		return newScriptedResponse(http.StatusNotModified, "", nil), nil
	}

	seedCallerConditionalCache(t, client, staleSeedHeader(""), "OLD")

	const callerETag = `"caller"`
	const callerDate = "Wed, 01 Jan 2020 00:00:00 GMT"
	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", callerETag)
	req.Header.Set("If-Modified-Since", callerDate)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	resp.Body.Close()

	seen := wire.calls[1].headers
	if got := seen.Get("If-None-Match"); got != callerETag {
		t.Fatalf("downstream If-None-Match = %q, want %q", got, callerETag)
	}
	if got := seen.Get("If-Modified-Since"); got != callerDate {
		t.Fatalf("downstream If-Modified-Since = %q, want %q", got, callerDate)
	}
	if values := seen["If-None-Match"]; len(values) != 1 || values[0] != callerETag {
		t.Fatalf("downstream If-None-Match values = %v, want [%s]", values, callerETag)
	}
	if values := seen["If-Modified-Since"]; len(values) != 1 || values[0] != callerDate {
		t.Fatalf("downstream If-Modified-Since values = %v, want [%s]", values, callerDate)
	}

	cached := readCachedEntry(t, cache, callerConditionalURL)
	if got := readBody(t, cached); got != "OLD" {
		t.Fatalf("cached body = %q, want OLD", got)
	}
}

// Weak ETags, multi-tag field values and multi-line headers must survive the
// trip byte-for-byte, and the caller's own Request/Header slices must not be
// mutated or replaced. (net/http stores header keys in canonical form by
// contract, so casing is exercised at the value/structure level rather than
// via non-canonical map keys, which Header.Get itself cannot see.)
func TestCallerConditionalPreservesHeaderShape(t *testing.T) {
	wire := &scriptedTransport{}
	client, _ := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("")), nil
		}
		return newScriptedResponse(http.StatusNotModified, "", nil), nil
	}

	seedCallerConditionalCache(t, client, staleSeedHeader(""), "OLD")

	const weakMultiTag = `W/"weak", "second"`
	const callerDate = "Wed, 01 Jan 2020 00:00:00 GMT"
	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	// Pre-allocated slices with a weak ETag, two tags in one field value and
	// an extra header line, so any rewrite or re-canonicalization is visible.
	inmSlice := []string{weakMultiTag, `"third"`}
	imsSlice := []string{callerDate}
	req.Header["If-None-Match"] = inmSlice
	req.Header["If-Modified-Since"] = imsSlice
	before := copyHeader(req.Header)

	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	resp.Body.Close()

	if !reflect.DeepEqual(req.Header, before) {
		t.Fatalf("caller headers mutated:\n before %v\n after  %v", before, req.Header)
	}
	// The exact slices must remain backed by the caller's data.
	if &req.Header["If-None-Match"][0] != &inmSlice[0] {
		t.Fatal("If-None-Match header slice was replaced")
	}
	if &req.Header["If-Modified-Since"][0] != &imsSlice[0] {
		t.Fatal("If-Modified-Since header slice was replaced")
	}
	// The downstream transport must have received the very same request.
	if wire.calls[1].req != req {
		t.Fatal("caching Transport replaced or cloned the caller request")
	}

	seen := wire.calls[1].headers
	if got := seen["If-None-Match"]; !reflect.DeepEqual(got, inmSlice) {
		t.Fatalf("downstream If-None-Match = %v, want %v", got, inmSlice)
	}
	if got := seen["If-Modified-Since"]; !reflect.DeepEqual(got, imsSlice) {
		t.Fatalf("downstream If-Modified-Since = %v, want %v", got, imsSlice)
	}
	// No validator derived from the cached entry may be attached in addition.
	if len(seen) != 2 {
		t.Fatalf("downstream header set = %v, want only the two caller validators", seen)
	}
}

// The caller may spell the header name in any case: Header.Set canonicalizes
// the key per the net/http contract, and detection must still recognize it as
// a caller-owned conditional request and forward the value untouched.
func TestCallerConditionalHeaderNameCasing(t *testing.T) {
	wire := &scriptedTransport{}
	client, _ := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("")), nil
		}
		return newScriptedResponse(http.StatusNotModified, "", nil), nil
	}

	seedCallerConditionalCache(t, client, staleSeedHeader(""), "OLD")

	for i, name := range []string{"if-none-match", "IF-NONE-MATCH", "If-None-Match"} {
		req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
		if err != nil {
			t.Fatal(err)
		}
		req.Header.Set(name, `"caller"`)
		resp, err := client.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		if resp.StatusCode != http.StatusNotModified {
			t.Fatalf("header name %q: status = %d, want 304", name, resp.StatusCode)
		}
		resp.Body.Close()
		// The seed plus one request per iteration, none served from cache.
		if len(wire.calls) != i+2 {
			t.Fatalf("header name %q: downstream calls = %d, want %d", name, len(wire.calls), i+2)
		}
		seen := wire.calls[i+1].headers
		if got := seen.Get("If-None-Match"); got != `"caller"` {
			t.Fatalf("header name %q: downstream If-None-Match = %q", name, got)
		}
		if got := seen.Get("If-Modified-Since"); got != "" {
			t.Fatalf("header name %q: If-Modified-Since = %q, want absent", name, got)
		}
	}
}

// After an explicit 304 leaves the cache alone, a plain GET (no caller
// conditions) still takes the automatic revalidation path: the cached
// validators are sent, the origin's 304 is merged back into the stored entity
// and end-to-end response headers are refreshed.
func TestCallerConditional304ThenPlainGetMerges(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		switch call {
		case 1:
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("")), nil
		case 2:
			return newScriptedResponse(http.StatusNotModified, "", http.Header{"Etag": {`"caller"`}}), nil
		default:
			h := http.Header{"X-Counter": {"7"}}
			return newScriptedResponse(http.StatusNotModified, "", h), nil
		}
	}

	seedCallerConditionalCache(t, client, staleSeedHeader(""), "OLD")

	// Explicit caller conditional: 304 passes through.
	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"caller"`)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("explicit conditional status = %d, want 304", resp.StatusCode)
	}
	resp.Body.Close()

	// Plain GET: Transport-generated revalidation should merge the 304.
	plain, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	resp, err = client.Do(plain)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("plain GET status = %d, want merged 200", resp.StatusCode)
	}
	if body := readBody(t, resp); body != "OLD" {
		t.Fatalf("plain GET body = %q, want OLD", body)
	}
	if got := resp.Header.Get(XFromCache); got != "1" {
		t.Fatalf("X-From-Cache = %q, want 1", got)
	}
	if got := resp.Header.Get("X-Counter"); got != "7" {
		t.Fatalf("merged X-Counter = %q, want 7 (end-to-end header not refreshed)", got)
	}

	if len(wire.calls) != 3 {
		t.Fatalf("downstream calls = %d, want 3", len(wire.calls))
	}
	if got := wire.calls[2].headers.Get("If-None-Match"); got != `"old"` {
		t.Fatalf("auto-revalidation If-None-Match = %q, want %q", got, `"old"`)
	}

	cached := readCachedEntry(t, cache, callerConditionalURL)
	if cached == nil {
		t.Fatal("cached entry missing after merged revalidation")
	}
	if got := readBody(t, cached); got != "OLD" {
		t.Fatalf("cached body = %q, want OLD", got)
	}
}

// A new 200 for a caller-owned conditional replaces the old entry once its
// body has been fully consumed, and is then served from cache normally.
func TestCallerConditional200FullReadReplacesCache(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", freshSeedHeader()), nil
		}
		return newScriptedResponse(http.StatusOK, "NEW", http.Header{
			"Date":          {time.Now().Format(time.RFC1123)},
			"Cache-Control": {"max-age=3600"},
			"Etag":          {`"new"`},
		}), nil
	}

	seedCallerConditionalCache(t, client, freshSeedHeader(), "OLD")

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"old"`)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", resp.StatusCode)
	}
	if body := readBody(t, resp); body != "NEW" {
		t.Fatalf("body = %q, want NEW", body)
	}
	resp.Body.Close()
	if got := resp.Header.Get(XFromCache); got != "" {
		t.Fatalf("X-From-Cache = %q, want absent for a wire response", got)
	}

	cached := readCachedEntry(t, cache, callerConditionalURL)
	if cached == nil {
		t.Fatal("new 200 was not stored")
	}
	if got := readBody(t, cached); got != "NEW" {
		t.Fatalf("cached body = %q, want NEW", got)
	}
	if got := cached.Header.Get("Etag"); got != `"new"` {
		t.Fatalf("cached etag = %q, want %q", got, `"new"`)
	}

	// The fresh replacement must now satisfy a plain GET without a wire call.
	plain, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	resp, err = client.Do(plain)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if body := readBody(t, resp); body != "NEW" {
		t.Fatalf("cached GET body = %q, want NEW", body)
	}
	if resp.Header.Get(XFromCache) != "1" {
		t.Fatal("plain GET was not served from the replaced cache entry")
	}
	if len(wire.calls) != 2 {
		t.Fatalf("downstream calls = %d, want 2", len(wire.calls))
	}
}

// A 200 whose body is only partially read is not cached yet: the old entry
// must remain intact and keep serving subsequent requests.
func TestCallerConditional200PartialReadKeepsOldCache(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", freshSeedHeader()), nil
		}
		return newScriptedResponse(http.StatusOK, "NEW-BODY-LONG", http.Header{
			"Date":          {time.Now().Format(time.RFC1123)},
			"Cache-Control": {"max-age=3600"},
			"Etag":          {`"new"`},
		}), nil
	}

	seedCallerConditionalCache(t, client, freshSeedHeader(), "OLD")

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"old"`)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	partial := make([]byte, 3)
	if n, err := io.ReadFull(resp.Body, partial); err != nil || n != 3 {
		t.Fatalf("partial read = %d, %v", n, err)
	}
	if string(partial) != "NEW" {
		t.Fatalf("partial body = %q, want NEW", partial)
	}
	resp.Body.Close()

	// EOF was never reached, so the old cache entry must be untouched.
	cached := readCachedEntry(t, cache, callerConditionalURL)
	if cached == nil {
		t.Fatal("old cache entry vanished after partial read")
	}
	if got := readBody(t, cached); got != "OLD" {
		t.Fatalf("cached body = %q, want OLD", got)
	}

	// The fresh old entry serves the next plain GET without another wire call.
	plain, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	resp, err = client.Do(plain)
	if err != nil {
		t.Fatal(err)
	}
	defer resp.Body.Close()
	if body := readBody(t, resp); body != "OLD" {
		t.Fatalf("plain GET body = %q, want OLD", body)
	}
	if resp.Header.Get(XFromCache) != "1" {
		t.Fatal("plain GET was not served from the surviving cache entry")
	}
	if len(wire.calls) != 2 {
		t.Fatalf("downstream calls = %d, want 2", len(wire.calls))
	}
}

// no-store on the caller-owned 200 still applies: the old entry is removed and
// the new response is not stored.
func TestCallerConditional200NoStoreDeletesOldCache(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", freshSeedHeader()), nil
		}
		return newScriptedResponse(http.StatusOK, "NEW", http.Header{
			"Date":          {time.Now().Format(time.RFC1123)},
			"Cache-Control": {"no-store"},
		}), nil
	}

	seedCallerConditionalCache(t, client, freshSeedHeader(), "OLD")

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"old"`)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if body := readBody(t, resp); body != "NEW" {
		t.Fatalf("body = %q, want NEW", body)
	}
	resp.Body.Close()

	if cached := readCachedEntry(t, cache, callerConditionalURL); cached != nil {
		t.Fatalf("no-store response was cached: %+v", cached)
	}

	// A plain GET must therefore go back downstream.
	plain, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := client.Do(plain); err != nil {
		t.Fatal(err)
	}
	if len(wire.calls) != 3 {
		t.Fatalf("downstream calls = %d, want 3 (deleted entry must not answer)", len(wire.calls))
	}
}

// Even with stale-if-error available, a transport error on a caller-owned
// conditional request must surface as the actual error rather than the stale
// body, and the cache entry must survive.
func TestCallerConditionalNetworkError(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	downErr := errors.New("simulated dial failure")
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("stale-if-error")), nil
		}
		return nil, downErr
	}

	seedCallerConditionalCache(t, client, staleSeedHeader("stale-if-error"), "OLD")

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"caller"`)
	req.Header.Set("Cache-Control", "stale-if-error")
	resp, err := client.Do(req)
	if !errors.Is(err, downErr) {
		t.Fatalf("err = %v, want %v", err, downErr)
	}
	if resp != nil {
		t.Fatalf("resp = %+v, want nil", resp)
	}

	if len(wire.calls) != 2 {
		t.Fatalf("downstream calls = %d, want 2", len(wire.calls))
	}
	if got := wire.calls[1].headers.Get("If-None-Match"); got != `"caller"` {
		t.Fatalf("downstream If-None-Match = %q, want %q", got, `"caller"`)
	}

	cached := readCachedEntry(t, cache, callerConditionalURL)
	if cached == nil {
		t.Fatal("cached entry deleted after transport error")
	}
	if got := readBody(t, cached); got != "OLD" {
		t.Fatalf("cached body = %q, want OLD", got)
	}
}

// A 5xx on a caller-owned conditional request is handed back as-is even when
// stale-if-error would allow serving the cached entity on the automatic path.
func TestCallerConditional5xx(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "OLD", staleSeedHeader("stale-if-error")), nil
		}
		return newScriptedResponse(http.StatusInternalServerError, "BOOM", http.Header{
			"Cache-Control": {"stale-if-error"},
		}), nil
	}

	seedCallerConditionalCache(t, client, staleSeedHeader("stale-if-error"), "OLD")

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"caller"`)
	req.Header.Set("Cache-Control", "stale-if-error")
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", resp.StatusCode)
	}
	if body := readBody(t, resp); body != "BOOM" {
		t.Fatalf("body = %q, want BOOM", body)
	}
	resp.Body.Close()
	if got := resp.Header.Get(XFromCache); got != "" {
		t.Fatalf("X-From-Cache = %q, want absent", got)
	}

	cached := readCachedEntry(t, cache, callerConditionalURL)
	if cached == nil || cached.StatusCode != http.StatusOK {
		t.Fatalf("cached entry changed after 5xx: %+v", cached)
	}
	if got := readBody(t, cached); got != "OLD" {
		t.Fatalf("cached body = %q, want OLD", got)
	}
}

// Without any cached entry, a caller-owned conditional request is still a pure
// passthrough: a 304 reaches the caller unchanged and nothing gets stored.
func TestCallerConditionalMissPassthrough304(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		return newScriptedResponse(http.StatusNotModified, "", nil), nil
	}

	req, err := http.NewRequest(http.MethodGet, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"caller"`)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("status = %d, want 304", resp.StatusCode)
	}
	if body := readBody(t, resp); body != "" {
		t.Fatalf("304 body = %q, want empty", body)
	}
	resp.Body.Close()
	if got := resp.Header.Get(XFromCache); got != "" {
		t.Fatalf("X-From-Cache = %q, want absent", got)
	}
	if len(wire.calls) != 1 {
		t.Fatalf("downstream calls = %d, want 1", len(wire.calls))
	}
	if cached := readCachedEntry(t, cache, callerConditionalURL); cached != nil {
		t.Fatalf("a bare 304 was stored: %+v", cached)
	}
}

// HEAD requests carrying caller validators follow the same passthrough rules
// as GET and never get merged into a cached HEAD response.
func TestCallerConditionalHead304(t *testing.T) {
	wire := &scriptedTransport{}
	client, cache := newTestClient(wire)
	wire.respond = func(call int, req *http.Request) (*http.Response, error) {
		if call == 1 {
			return newScriptedResponse(http.StatusOK, "", staleSeedHeader("")), nil
		}
		return newScriptedResponse(http.StatusNotModified, "", nil), nil
	}

	seedReq, err := http.NewRequest(http.MethodHead, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	seedResp, err := client.Do(seedReq)
	if err != nil {
		t.Fatal(err)
	}
	seedResp.Body.Close()

	req, err := http.NewRequest(http.MethodHead, callerConditionalURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("If-None-Match", `"caller"`)
	resp, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	if resp.StatusCode != http.StatusNotModified {
		t.Fatalf("HEAD status = %d, want 304", resp.StatusCode)
	}
	resp.Body.Close()
	if got := resp.Header.Get(XFromCache); got != "" {
		t.Fatalf("X-From-Cache = %q, want absent", got)
	}
	if len(wire.calls) != 2 {
		t.Fatalf("downstream calls = %d, want 2", len(wire.calls))
	}
	if got := wire.calls[1].headers.Get("If-None-Match"); got != `"caller"` {
		t.Fatalf("downstream If-None-Match = %q, want %q", got, `"caller"`)
	}

	cached := readCachedEntry(t, cache, "HEAD "+callerConditionalURL)
	if cached == nil || cached.StatusCode != http.StatusOK {
		t.Fatalf("cached HEAD entry changed: %+v", cached)
	}
}
