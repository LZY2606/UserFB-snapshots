package regress_test

import (
	"bytes"
	"errors"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	h "github.com/gregjones/httpcache"
)

const varyTestURL = "http://example.test/page"

// scriptedRT is an in-process http.RoundTripper: no sockets are needed, and
// every response (including body delivery and call ordering) is controlled by
// the test.
type scriptedRT struct {
	mu     sync.Mutex
	calls  int32
	seen   []*http.Request
	enter  func(call int32)
	handle func(call int32, req *http.Request) (*http.Response, error)
}

func (rt *scriptedRT) RoundTrip(req *http.Request) (*http.Response, error) {
	call := atomic.AddInt32(&rt.calls, 1)
	rt.mu.Lock()
	rt.seen = append(rt.seen, req.Clone(req.Context()))
	enter := rt.enter
	rt.mu.Unlock()
	if enter != nil {
		enter(call)
	}
	return rt.handle(call, req)
}

func (rt *scriptedRT) originCalls() int { return int(atomic.LoadInt32(&rt.calls)) }

func (rt *scriptedRT) requestHeaders(call int) http.Header {
	rt.mu.Lock()
	defer rt.mu.Unlock()
	return rt.seen[call-1].Header.Clone()
}

func newTestTransport(rt http.RoundTripper) (*h.Transport, *h.MemoryCache) {
	cache := h.NewMemoryCache()
	tr := &h.Transport{
		Transport:           rt,
		Cache:               cache,
		MarkCachedResponses: true,
		VaryVariants:        true,
	}
	return tr, cache
}

func scriptedResponse(status int, header http.Header, body io.ReadCloser) *http.Response {
	if header == nil {
		header = http.Header{}
	}
	headers := header.Clone()
	if headers.Get("Date") == "" {
		headers.Set("Date", time.Now().UTC().Format(time.RFC1123))
	}
	return &http.Response{
		Status:        http.StatusText(status),
		StatusCode:    status,
		Proto:         "HTTP/1.1",
		ProtoMajor:    1,
		ProtoMinor:    1,
		Header:        headers,
		Body:          body,
		ContentLength: -1,
	}
}

func stringResponse(status int, header http.Header, body string) *http.Response {
	return scriptedResponse(status, header, io.NopCloser(bytes.NewReader([]byte(body))))
}

func varyHeaders(vary string, extra ...[2]string) http.Header {
	header := http.Header{}
	header.Set("Vary", vary)
	for _, kv := range extra {
		header.Set(kv[0], kv[1])
	}
	return header
}

func doGet(t *testing.T, client *http.Client, headers [][2]string) (string, *http.Response) {
	t.Helper()
	req, err := http.NewRequest("GET", varyTestURL, nil)
	if err != nil {
		t.Fatal(err)
	}
	for _, kv := range headers {
		req.Header.Set(kv[0], kv[1])
	}
	res, err := client.Do(req)
	if err != nil {
		t.Fatal(err)
	}
	b, err := io.ReadAll(res.Body)
	res.Body.Close()
	if err != nil {
		t.Fatalf("reading body: %v", err)
	}
	return string(b), res
}

func langHeader(lang string) [][2]string {
	return [][2]string{{"Accept-Language", lang}}
}

// blockingBody holds its first Read until release is closed, after signaling
// started. It lets tests arrange EOF publication order with channels instead
// of sleeping.
type blockingBody struct {
	data    []byte
	release <-chan struct{}
	started chan<- struct{}
	once    sync.Once
	buf     bytes.Buffer
}

func (b *blockingBody) Read(p []byte) (int, error) {
	b.once.Do(func() {
		close(b.started)
		if b.release != nil {
			<-b.release
		}
		b.buf.Write(b.data)
	})
	return b.buf.Read(p)
}

func (b *blockingBody) Close() error { return nil }

// errorBody serves data once, then fails with err on the next read.
type errorBody struct {
	data []byte
	err  error
	pos  int
}

func (b *errorBody) Read(p []byte) (int, error) {
	if b.pos >= len(b.data) {
		return 0, b.err
	}
	n := copy(p, b.data[b.pos:])
	b.pos += n
	return n, nil
}

func (b *errorBody) Close() error { return nil }

func notModified(header http.Header) *http.Response {
	if header == nil {
		header = http.Header{}
	}
	return scriptedResponse(http.StatusNotModified, header, http.NoBody)
}

func reqWithLang(lang string) *http.Request {
	req, _ := http.NewRequest("GET", varyTestURL, nil)
	req.Header.Set("Accept-Language", lang)
	return req
}

func TestVaryCatalog(t *testing.T) {
	t.Run("two languages share one cache entry", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), lang), nil
		}}
		tr, _ := newTestTransport(rt)
		client := tr.Client()

		wantCached := []bool{false, true, false, true}
		for i, lang := range []string{"en", "en", "fr", "en"} {
			body, res := doGet(t, client, langHeader(lang))
			if body != lang {
				t.Fatalf("body=%q want %q", body, lang)
			}
			gotMark := res.Header.Get("X-From-Cache") == "1"
			if gotMark != wantCached[i] {
				t.Fatalf("request %d (%s) cached mark=%v want %v", i, lang, gotMark, wantCached[i])
			}
		}
		if rt.originCalls() != 2 {
			t.Fatalf("origin calls=%d want 2", rt.originCalls())
		}
	})

	t.Run("entry bytes survive a brand new cache and transport", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), lang), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))

		raw, ok := cache.Get(varyTestURL)
		if !ok {
			t.Fatal("nothing stored under url key")
		}
		if !bytes.HasPrefix(raw, []byte("HTTPCCAT1\n")) {
			t.Fatal("entry is not a vary catalog")
		}
		// Persist the raw bytes outside the process cache and reload them.
		path := filepath.Join(t.TempDir(), "entry.bin")
		if err := os.WriteFile(path, raw, 0o600); err != nil {
			t.Fatal(err)
		}
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		reloaded := h.NewMemoryCache()
		reloaded.Set(varyTestURL, data)

		rt2 := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			t.Fatal("reloaded cache must not hit the origin")
			return nil, nil
		}}
		tr2 := &h.Transport{Transport: rt2, Cache: reloaded, MarkCachedResponses: true, VaryVariants: true}
		client2 := tr2.Client()
		for _, lang := range []string{"en", "fr", "fr", "en"} {
			body, _ := doGet(t, client2, langHeader(lang))
			if body != lang {
				t.Fatalf("after reload body=%q want %q", body, lang)
			}
		}
		if rt2.originCalls() != 0 {
			t.Fatalf("reloaded cache made %d origin calls", rt2.originCalls())
		}
	})

	t.Run("missing field differs from explicit empty value", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			tag, ok := req.Header["X-Tag"]
			body := "missing"
			if ok {
				body = "empty:" + stringsJoin(tag)
			}
			return stringResponse(200, varyHeaders("X-Tag",
				[2]string{"Cache-Control", "max-age=3600"}), body), nil
		}}
		tr, _ := newTestTransport(rt)
		client := tr.Client()

		missingReq, _ := http.NewRequest("GET", varyTestURL, nil)
		emptyReq, _ := http.NewRequest("GET", varyTestURL, nil)
		emptyReq.Header["X-Tag"] = []string{""}
		multiReq, _ := http.NewRequest("GET", varyTestURL, nil)
		multiReq.Header["X-Tag"] = []string{"a", "b"}

		fetch := func(req *http.Request) string {
			res, err := client.Do(req)
			if err != nil {
				t.Fatal(err)
			}
			b, _ := io.ReadAll(res.Body)
			res.Body.Close()
			return string(b)
		}
		if got := fetch(missingReq); got != "missing" {
			t.Fatalf("missing got %q", got)
		}
		if got := fetch(emptyReq); got != "empty:" {
			t.Fatalf("empty got %q", got)
		}
		if got := fetch(multiReq); got != "empty:a,b" {
			t.Fatalf("multi got %q", got)
		}
		// Second round: all three must be served from cache and stay distinct.
		if got := fetch(missingReq); got != "missing" {
			t.Fatalf("cached missing got %q", got)
		}
		if got := fetch(emptyReq); got != "empty:" {
			t.Fatalf("cached empty got %q", got)
		}
		if got := fetch(multiReq); got != "empty:a,b" {
			t.Fatalf("cached multi got %q", got)
		}
		if rt.originCalls() != 3 {
			t.Fatalf("origin calls=%d want 3", rt.originCalls())
		}

		// Values are case-sensitive; surrounding spaces and tabs are trimmed.
		upper, _ := http.NewRequest("GET", varyTestURL, nil)
		upper.Header.Set("X-Tag", "EN")
		if got := fetch(upper); got != "empty:EN" {
			t.Fatalf("uppercase got %q", got)
		}
		if got, _ := doGet(t, client, [][2]string{{"X-Tag", "en"}}); got != "empty:en" {
			t.Fatalf("plain en got %q", got)
		}
		if got, _ := doGet(t, client, [][2]string{{"X-Tag", " en\t"}}); got != "empty:en" {
			t.Fatalf("padded en should trim to en, got %q", got)
		}
		if rt.originCalls() != 5 {
			t.Fatalf("origin calls=%d want 5", rt.originCalls())
		}
	})

	t.Run("wildcard vary is returned but never reused", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			return stringResponse(200, varyHeaders("*",
				[2]string{"Cache-Control", "max-age=3600"}), "wild"), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		body, _ := doGet(t, client, langHeader("en"))
		if body != "wild" {
			t.Fatalf("body=%q", body)
		}
		doGet(t, client, langHeader("en"))
		if rt.originCalls() != 2 {
			t.Fatalf("wildcard entry was reused: origin calls=%d want 2", rt.originCalls())
		}
		req, _ := http.NewRequest("GET", varyTestURL, nil)
		if resp, err := h.CachedResponse(cache, req); err != nil {
			t.Fatal(err)
		} else if resp != nil {
			t.Fatal("CachedResponse must return nil for a Vary: * entry")
		}
	})

	t.Run("vary header lines merge, dedup and ignore empty tokens", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			header := http.Header{}
			header.Add("Vary", "X-A, x-a")
			header.Add("Vary", ", X-B")
			header.Set("Cache-Control", "max-age=3600")
			return stringResponse(200, header, req.Header.Get("X-A")+"/"+req.Header.Get("X-B")), nil
		}}
		tr, _ := newTestTransport(rt)
		client := tr.Client()
		first, res := doGet(t, client, [][2]string{{"X-A", "1"}, {"X-B", "2"}})
		if first != "1/2" {
			t.Fatalf("body=%q", first)
		}
		if res.Header.Get("X-From-Cache") != "" {
			t.Fatal("first response must not be marked cached")
		}
		if body, hit := doGet(t, client, [][2]string{{"X-A", "1"}, {"X-B", "2"}}); body != "1/2" || hit.Header.Get("X-From-Cache") != "1" {
			t.Fatalf("exact repeat not served from cache: body=%q mark=%q", body, hit.Header.Get("X-From-Cache"))
		}
		doGet(t, client, [][2]string{{"X-A", "1"}, {"X-B", "3"}}) // different X-B: miss
		reqMissingA, _ := http.NewRequest("GET", varyTestURL, nil)
		reqMissingA.Header.Set("X-B", "2")
		res2, err := client.Do(reqMissingA) // missing X-A: miss
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res2.Body)
		res2.Body.Close()
		if rt.originCalls() != 3 {
			t.Fatalf("origin calls=%d want 3", rt.originCalls())
		}
	})

	t.Run("CachedResponse selects the matching representation or nil", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), lang), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))

		cached := func(lang string) []byte {
			req, _ := http.NewRequest("GET", varyTestURL, nil)
			if lang != "" {
				req.Header.Set("Accept-Language", lang)
			}
			resp, err := h.CachedResponse(cache, req)
			if err != nil {
				t.Fatal(err)
			}
			if resp == nil {
				return nil
			}
			b, _ := io.ReadAll(resp.Body)
			resp.Body.Close()
			return b
		}
		if string(cached("en")) != "en" || string(cached("fr")) != "fr" {
			t.Fatal("CachedResponse picked the wrong representation")
		}
		if cached("de") != nil {
			t.Fatal("CachedResponse must return nil on miss")
		}
	})

	t.Run("default mode keeps single response storage", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), lang), nil
		}}
		cache := h.NewMemoryCache()
		tr := &h.Transport{Transport: rt, Cache: cache, MarkCachedResponses: true}
		client := tr.Client()
		for _, lang := range []string{"en", "en", "fr", "en"} {
			body, _ := doGet(t, client, langHeader(lang))
			if body != lang {
				t.Fatalf("body=%q want %q", body, lang)
			}
		}
		if rt.originCalls() != 3 {
			t.Fatalf("default mode origin calls=%d want 3", rt.originCalls())
		}
		raw, ok := cache.Get(varyTestURL)
		if !ok || bytes.HasPrefix(raw, []byte("HTTPCCAT1\n")) {
			t.Fatal("default mode must keep writing legacy single-response entries")
		}
	})

	t.Run("new mode reuses and conservatively rejects legacy entries", func(t *testing.T) {
		// Write the legacy entry with the real default mode, not by hand.
		legacyRT := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), "en"), nil
		}}
		cache := h.NewMemoryCache()
		legacyTr := &h.Transport{Transport: legacyRT, Cache: cache, MarkCachedResponses: true}
		doGet(t, legacyTr.Client(), langHeader("en"))

		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), lang), nil
		}}
		tr := &h.Transport{Transport: rt, Cache: cache, MarkCachedResponses: true, VaryVariants: true}
		client := tr.Client()

		// Same condition: the legacy representation is validated and used.
		if body, res := doGet(t, client, langHeader("en")); body != "en" || res.Header.Get("X-From-Cache") != "1" {
			t.Fatalf("legacy entry not reused: body=%q mark=%q", body, res.Header.Get("X-From-Cache"))
		}
		if rt.originCalls() != 0 {
			t.Fatalf("legacy reuse made %d origin calls", rt.originCalls())
		}

		// Multi-valued request header: old format cannot record it, conservative miss.
		multi, _ := http.NewRequest("GET", varyTestURL, nil)
		multi.Header["Accept-Language"] = []string{"en", "fr"}
		res, err := client.Do(multi)
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res.Body)
		res.Body.Close()
		// Explicit empty value: conservative miss.
		doGet(t, client, [][2]string{{"Accept-Language", ""}})
		// Missing field while the legacy entry recorded a non-empty value: miss.
		reqMissing, _ := http.NewRequest("GET", varyTestURL, nil)
		res3, err := client.Do(reqMissing)
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res3.Body)
		res3.Body.Close()
		if rt.originCalls() != 3 {
			t.Fatalf("origin calls=%d want 3 (all ambiguous cases must re-fetch)", rt.originCalls())
		}
	})

	t.Run("no cross-language validators or stale bodies", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			if lang == "de" {
				if req.Header.Get("If-None-Match") != "" {
					t.Fatalf("request without a matching representation carried validator %q", req.Header.Get("If-None-Match"))
				}
			}
			header := varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "no-cache, stale-if-error"},
				[2]string{"Etag", `"e-` + lang + `"`})
			return stringResponse(200, header, lang), nil
		}}
		tr, _ := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))

		// Stale revalidation of each representation uses its own ETag.
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))
		if got := rt.requestHeaders(3).Get("If-None-Match"); got != `"e-en"` {
			t.Fatalf("en validator=%q", got)
		}
		if got := rt.requestHeaders(4).Get("If-None-Match"); got != `"e-fr"` {
			t.Fatalf("fr validator=%q", got)
		}

		// Unknown language + origin failure: must not borrow the en body or its validator.
		var deConditional http.Header
		rt.handle = func(call int32, req *http.Request) (*http.Response, error) {
			if lang := req.Header.Get("Accept-Language"); lang == "de" {
				deConditional = req.Header.Clone()
				return nil, errors.New("origin down")
			}
			// Matched representations fail too, but may use stale-if-error.
			return scriptedResponse(503, http.Header{}, http.NoBody), nil
		}
		req, _ := http.NewRequest("GET", varyTestURL, nil)
		req.Header.Set("Accept-Language", "de")
		if _, err := client.Do(req); err == nil {
			t.Fatal("expected transport error, got a borrowed cached body")
		}
		if deConditional.Get("If-None-Match") != "" || deConditional.Get("If-Modified-Since") != "" {
			t.Fatalf("unmatched request carried conditional headers: %#v", deConditional)
		}

		// Same failure for the matched en representation serves the stale body.
		reqEn, _ := http.NewRequest("GET", varyTestURL, nil)
		reqEn.Header.Set("Accept-Language", "en")
		res, err := client.Do(reqEn)
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(res.Body)
		res.Body.Close()
		if string(b) != "en" {
			t.Fatalf("stale-if-error returned %q want en", b)
		}
	})

	t.Run("range and conditional requests bypass the catalog", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), "plain"), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()

		rangeReq, _ := http.NewRequest("GET", varyTestURL, nil)
		rangeReq.Header.Set("Range", "bytes=0-3")
		res, err := client.Do(rangeReq)
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res.Body)
		res.Body.Close()
		if _, ok := cache.Get(varyTestURL); ok {
			t.Fatal("range request must not create a catalog entry")
		}

		inmReq, _ := http.NewRequest("GET", varyTestURL, nil)
		inmReq.Header.Set("If-None-Match", `"caller-etag"`)
		res2, err := client.Do(inmReq)
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res2.Body)
		res2.Body.Close()
		raw, ok := cache.Get(varyTestURL)
		if !ok || bytes.HasPrefix(raw, []byte("HTTPCCAT1\n")) {
			t.Fatal("conditional GET must use the legacy single-response path")
		}

		postReq, _ := http.NewRequest("POST", varyTestURL, nil)
		res3, err := client.Do(postReq)
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res3.Body)
		res3.Body.Close()
	})
}

func TestVaryRevalidation(t *testing.T) {
	t.Run("only the matched representation is validated and updated", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			if call == 3 {
				if req.Header.Get("If-None-Match") != `"e-en"` {
					t.Fatalf("call 3 validator=%q want e-en", req.Header.Get("If-None-Match"))
				}
				header := http.Header{}
				header.Set("X-Counter", "2")
				header.Set("Etag", `"e-en-v2"`)
				return notModified(header), nil
			}
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "no-cache"},
				[2]string{"Etag", `"e-` + lang + `"`}), lang), nil
		}}
		tr, _ := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))

		body, res := doGet(t, client, langHeader("en"))
		if body != "en" {
			t.Fatalf("revalidated body=%q want en", body)
		}
		if res.Header.Get("X-Counter") != "2" {
			t.Fatalf("304 header not merged: X-Counter=%q", res.Header.Get("X-Counter"))
		}
		if res.Header.Get("Etag") != `"e-en-v2"` {
			t.Fatalf("etag not updated: %q", res.Header.Get("Etag"))
		}
		if res.Header.Get("X-From-Cache") != "1" {
			t.Fatal("revalidated response should be marked as cached")
		}

		// fr keeps its own validator and body; en's 304 must not touch it.
		bodyFr, resFr := doGet(t, client, langHeader("fr"))
		if bodyFr != "fr" {
			t.Fatalf("fr body=%q", bodyFr)
		}
		if got := rt.requestHeaders(4).Get("If-None-Match"); got != `"e-fr"` {
			t.Fatalf("fr validator=%q want e-fr", got)
		}
		if resFr.Header.Get("X-Counter") != "" {
			t.Fatal("fr representation inherited en's 304 header")
		}
		// Next en revalidation uses the updated ETag.
		doGet(t, client, langHeader("en"))
		if got := rt.requestHeaders(5).Get("If-None-Match"); got != `"e-en-v2"` {
			t.Fatalf("en re-validation used %q want e-en-v2", got)
		}
	})

	t.Run("unmatched language sends no validator and cannot borrow stale bodies", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			if call == 3 {
				if req.Header.Get("If-None-Match") != "" || req.Header.Get("If-Modified-Since") != "" {
					t.Fatalf("unmatched request carried conditional headers: %#v", req.Header)
				}
				return scriptedResponse(500, varyHeaders("Accept-Language",
					[2]string{"Cache-Control", "no-cache, stale-if-error"}),
					http.NoBody), nil
			}
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "no-cache, stale-if-error"},
				[2]string{"Etag", `"e-` + lang + `"`}), lang), nil
		}}
		tr, _ := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))

		req, _ := http.NewRequest("GET", varyTestURL, nil)
		req.Header.Set("Accept-Language", "de")
		res, err := client.Do(req)
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(res.Body)
		res.Body.Close()
		if res.StatusCode != 500 {
			t.Fatalf("status=%d want 500", res.StatusCode)
		}
		if string(b) != "" {
			t.Fatalf("unmatched language got borrowed body %q", b)
		}
		if rt.originCalls() != 3 {
			t.Fatalf("origin calls=%d want 3", rt.originCalls())
		}

		// The matched en representation may still answer 5xx stale.
		rt.handle = func(call int32, req *http.Request) (*http.Response, error) {
			if req.Header.Get("If-None-Match") == "" {
				t.Fatal("matched revalidation lost its validator")
			}
			return scriptedResponse(503, http.Header{}, http.NoBody), nil
		}
		res2, err := client.Do(reqWithLang("en"))
		if err != nil {
			t.Fatal(err)
		}
		b2, _ := io.ReadAll(res2.Body)
		res2.Body.Close()
		if string(b2) != "en" {
			t.Fatalf("stale-if-error body=%q want en", b2)
		}
	})

	t.Run("304 changing the Vary set rebuilds matching conditions", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			switch call {
			case 3:
				if req.Header.Get("If-None-Match") != `"e-en"` {
					t.Fatalf("validator=%q", req.Header.Get("If-None-Match"))
				}
				header := http.Header{}
				header.Set("Vary", "Accept-Language, X-Tenant")
				header.Set("Etag", `"e-en-v2"`)
				header.Set("Cache-Control", "no-cache")
				return notModified(header), nil
			default:
				lang := req.Header.Get("Accept-Language")
				header := varyHeaders("Accept-Language",
					[2]string{"Cache-Control", "no-cache"},
					[2]string{"Etag", `"e-` + lang + `"`})
				if call >= 4 {
					header.Set("Vary", "Accept-Language, X-Tenant")
				}
				return stringResponse(200, header, lang), nil
			}
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))

		body, _ := doGet(t, client, langHeader("en"))
		if body != "en" {
			t.Fatalf("revalidated body=%q", body)
		}

		// Old-set fr variant must have been withdrawn.
		frReq, _ := http.NewRequest("GET", varyTestURL, nil)
		frReq.Header.Set("Accept-Language", "fr")
		if resp, err := h.CachedResponse(cache, frReq); err != nil {
			t.Fatal(err)
		} else if resp != nil {
			t.Fatal("old Vary set representation was not invalidated")
		}

		// fr now misses and must not reuse en's validator.
		doGet(t, client, langHeader("fr"))
		if got := rt.requestHeaders(4).Get("If-None-Match"); got != "" {
			t.Fatalf("fr miss carried validator %q", got)
		}
		// en is matched under the new set (X-Tenant missing), with new ETag.
		doGet(t, client, langHeader("en"))
		if got := rt.requestHeaders(5).Get("If-None-Match"); got != `"e-en-v2"` {
			t.Fatalf("en validator under new set=%q", got)
		}
		// A tenant value distinguishes a second representation under the new set.
		tenantReq, _ := http.NewRequest("GET", varyTestURL, nil)
		tenantReq.Header.Set("Accept-Language", "en")
		tenantReq.Header.Set("X-Tenant", "premium")
		res, err := client.Do(tenantReq)
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res.Body)
		res.Body.Close()
		if rt.originCalls() != 6 {
			t.Fatalf("origin calls=%d want 6", rt.originCalls())
		}
	})

	t.Run("304 redeclaring Vary star withdraws the whole url", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			if call == 3 {
				header := http.Header{}
				header.Set("Vary", "*")
				return notModified(header), nil
			}
			lang := req.Header.Get("Accept-Language")
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "no-cache"},
				[2]string{"Etag", `"e-` + lang + `"`}), lang), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))

		// The validated body is still returned for this one request.
		if body, _ := doGet(t, client, langHeader("en")); body != "en" {
			t.Fatalf("body=%q want en", body)
		}
		req, _ := http.NewRequest("GET", varyTestURL, nil)
		req.Header.Set("Accept-Language", "en")
		if resp, err := h.CachedResponse(cache, req); err != nil {
			t.Fatal(err)
		} else if resp != nil {
			t.Fatal("Vary: * entry must not be reusable")
		}
		// Every later request goes to the origin without stale validators.
		doGet(t, client, langHeader("fr"))
		if got := rt.requestHeaders(4).Get("If-None-Match"); got != "" {
			t.Fatalf("wildcard withdrawal left validator %q", got)
		}
		doGet(t, client, langHeader("en"))
		if rt.originCalls() != 5 {
			t.Fatalf("origin calls=%d want 5", rt.originCalls())
		}
	})
}

// TestVaryPublication covers the publication rules: full-body gating, EOF
// ordering, Vary set switches, late old bodies, partial and failed reads.
// Every ordering point is driven by channels.
func TestVaryPublication(t *testing.T) {
	t.Run("concurrent reads keep both representations in either EOF order", func(t *testing.T) {
		for _, frFirst := range []bool{true, false} {
			started := map[string]chan struct{}{"en": make(chan struct{}), "fr": make(chan struct{})}
			release := map[string]chan struct{}{"en": make(chan struct{}), "fr": make(chan struct{})}
			rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
				lang := req.Header.Get("Accept-Language")
				return scriptedResponse(200, varyHeaders("Accept-Language",
					[2]string{"Cache-Control", "max-age=3600"}),
					&blockingBody{data: []byte(lang), started: started[lang], release: release[lang]}), nil
			}}
			tr, _ := newTestTransport(rt)
			client := tr.Client()

			type result struct {
				lang string
				body string
				err  error
			}
			results := make(chan result, 2)
			fetch := func(lang string) {
				req, _ := http.NewRequest("GET", varyTestURL, nil)
				req.Header.Set("Accept-Language", lang)
				res, err := client.Do(req)
				if err != nil {
					results <- result{lang: lang, err: err}
					return
				}
				b, err := io.ReadAll(res.Body)
				res.Body.Close()
				results <- result{lang: lang, body: string(b), err: err}
			}
			go fetch("en")
			go fetch("fr")
			<-started["en"]
			<-started["fr"]

			order := []string{"en", "fr"}
			if frFirst {
				order = []string{"fr", "en"}
			}
			for _, lang := range order {
				close(release[lang])
			}
			for i := 0; i < 2; i++ {
				r := <-results
				if r.err != nil || r.body != r.lang {
					t.Fatalf("frFirst=%v got %+v", frFirst, r)
				}
			}

			// Both languages now hit without touching the origin again.
			for _, lang := range []string{"en", "fr", "fr", "en"} {
				body, _ := doGet(t, client, langHeader(lang))
				if body != lang {
					t.Fatalf("frFirst=%v cached body=%q want %q", frFirst, body, lang)
				}
			}
			if rt.originCalls() != 2 {
				t.Fatalf("frFirst=%v origin calls=%d want 2", frFirst, rt.originCalls())
			}
		}
	})

	t.Run("a fully read new language never overwrites the existing one", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), lang), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))
		doGet(t, client, langHeader("en"))
		if rt.originCalls() != 2 {
			t.Fatalf("origin calls=%d want 2", rt.originCalls())
		}
		enReq, _ := http.NewRequest("GET", varyTestURL, nil)
		enReq.Header.Set("Accept-Language", "en")
		enResp, err := h.CachedResponse(cache, enReq)
		if err != nil || enResp == nil {
			t.Fatalf("en representation lost: resp=%v err=%v", enResp, err)
		}
		b, _ := io.ReadAll(enResp.Body)
		enResp.Body.Close()
		if string(b) != "en" {
			t.Fatalf("en body=%q", b)
		}
	})

	t.Run("late old-set body cannot republish a withdrawn set", func(t *testing.T) {
		enteredOld := make(chan struct{})
		releaseOldCall := make(chan struct{})
		newBodyStarted := make(chan struct{})
		releaseNewBody := make(chan struct{})
		oldBodyStarted := make(chan struct{})
		releaseOldBody := make(chan struct{})

		rt := &scriptedRT{
			enter: func(call int32) {
				if call == 2 {
					close(enteredOld)
					<-releaseOldCall
				}
			},
			handle: func(call int32, req *http.Request) (*http.Response, error) {
				switch call {
				case 1:
					return stringResponse(200, varyHeaders("X-A",
						[2]string{"Cache-Control", "no-cache"},
						[2]string{"Etag", `"e-en"`}), "old-en"), nil
				case 2:
					// Stale revalidation of the old en representation, answered
					// with a 200 that still declares the old Vary set.
					return scriptedResponse(200, varyHeaders("X-A",
						[2]string{"Cache-Control", "no-cache"}),
						&blockingBody{data: []byte("late-en"), started: oldBodyStarted, release: releaseOldBody}), nil
				default:
					// The newer request switches the URL to Vary: X-B.
					return scriptedResponse(200, varyHeaders("X-B",
						[2]string{"Cache-Control", "max-age=3600"}),
						&blockingBody{data: []byte("new-set"), started: newBodyStarted, release: releaseNewBody}), nil
				}
			},
		}
		tr, cache := newTestTransport(rt)
		client := tr.Client()

		oldReq, _ := http.NewRequest("GET", varyTestURL, nil)
		oldReq.Header.Set("X-A", "1")
		res, err := client.Do(oldReq)
		if err != nil {
			t.Fatal(err)
		}
		io.Copy(io.Discard, res.Body)
		res.Body.Close()

		type fetchResult struct {
			body string
			err  error
		}
		oldDone := make(chan fetchResult, 1)
		go func() {
			r, err := client.Do(oldReq)
			if err != nil {
				oldDone <- fetchResult{err: err}
				return
			}
			b, err := io.ReadAll(r.Body)
			r.Body.Close()
			oldDone <- fetchResult{body: string(b), err: err}
		}()
		<-enteredOld

		// Newer downstream request (generation follows call ordering, not clocks).
		newReq, _ := http.NewRequest("GET", varyTestURL, nil)
		newReq.Header.Set("X-B", "9")
		newResp, err := client.Do(newReq)
		if err != nil {
			t.Fatal(err)
		}
		// Let the old call reach its body and park there, still unpublished.
		close(releaseOldCall)
		<-oldBodyStarted

		// Read the new response in a goroutine so its body can park too.
		newDone := make(chan fetchResult, 1)
		go func() {
			b, err := io.ReadAll(newResp.Body)
			newResp.Body.Close()
			newDone <- fetchResult{body: string(b), err: err}
		}()
		<-newBodyStarted

		// New set publishes first and withdraws the old set's barriers.
		close(releaseNewBody)
		newResult := <-newDone
		if newResult.err != nil || newResult.body != "new-set" {
			t.Fatalf("new body=%q err=%v", newResult.body, newResult.err)
		}
		// Now let the stale old response finish reading; it must be discarded.
		close(releaseOldBody)
		old := <-oldDone
		if old.err != nil || old.body != "late-en" {
			t.Fatalf("old in-flight response changed: %+v", old)
		}

		// The old-set condition must be gone, even though its body finished last.
		if resp, err := h.CachedResponse(cache, oldReq); err != nil {
			t.Fatal(err)
		} else if resp != nil {
			t.Fatal("old Vary set came back after a late read")
		}
		// And the new-set representation is served from cache.
		if body, hit := doGet(t, client, [][2]string{{"X-B", "9"}}); body != "new-set" || hit.Header.Get("X-From-Cache") != "1" {
			t.Fatalf("new set not cached: body=%q mark=%q", body, hit.Header.Get("X-From-Cache"))
		}
		if rt.originCalls() != 3 {
			t.Fatalf("origin calls=%d want 3", rt.originCalls())
		}
	})

	t.Run("same-set publication replaces only the matching variant", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			// en always revalidates; fr stays fresh, so replacing en must not
			// disturb the independently cached fr representation.
			cacheControl := "no-cache"
			if lang == "fr" {
				cacheControl = "max-age=3600"
			}
			body := lang
			if call == 3 {
				body = "en-v2"
			}
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", cacheControl}), body), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))
		doGet(t, client, langHeader("fr"))
		if body, _ := doGet(t, client, langHeader("en")); body != "en-v2" {
			t.Fatalf("en republished body=%q want en-v2", body)
		}
		// fr survives the en replacement and is still a fresh cache hit.
		if body, hit := doGet(t, client, langHeader("fr")); body != "fr" || hit.Header.Get("X-From-Cache") != "1" {
			t.Fatalf("fr variant damaged: body=%q mark=%q", body, hit.Header.Get("X-From-Cache"))
		}
		if rt.originCalls() != 3 {
			t.Fatalf("origin calls=%d want 3", rt.originCalls())
		}
		enReq, _ := http.NewRequest("GET", varyTestURL, nil)
		enReq.Header.Set("Accept-Language", "en")
		enResp, _ := h.CachedResponse(cache, enReq)
		b, _ := io.ReadAll(enResp.Body)
		enResp.Body.Close()
		if string(b) != "en-v2" {
			t.Fatalf("cached en body=%q want en-v2", b)
		}
	})

	t.Run("partial and failed reads publish nothing and keep the old set", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			lang := req.Header.Get("Accept-Language")
			if lang == "broken" {
				return scriptedResponse(200, varyHeaders("Accept-Language",
					[2]string{"Cache-Control", "max-age=3600"}),
					&errorBody{data: []byte("partial"), err: errors.New("connection reset")}), nil
			}
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "max-age=3600"}), lang), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))

		// Error read: no half representation may be stored.
		errReq, _ := http.NewRequest("GET", varyTestURL, nil)
		errReq.Header.Set("Accept-Language", "broken")
		res, err := client.Do(errReq)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := io.ReadAll(res.Body); err == nil {
			t.Fatal("expected the injected read error")
		}
		res.Body.Close()

		// Partial read followed by early Close: also stores nothing.
		partReq, _ := http.NewRequest("GET", varyTestURL, nil)
		partReq.Header.Set("Accept-Language", "partial")
		res2, err := client.Do(partReq)
		if err != nil {
			t.Fatal(err)
		}
		buf := make([]byte, 2)
		if n, err := res2.Body.Read(buf); err != nil || n != 2 || string(buf) != "pa" {
			t.Fatalf("partial read n=%d err=%v", n, err)
		}
		res2.Body.Close()

		// The already published en representation is untouched and still fresh.
		if body, hit := doGet(t, client, langHeader("en")); body != "en" || hit.Header.Get("X-From-Cache") != "1" {
			t.Fatalf("published set damaged: body=%q mark=%q", body, hit.Header.Get("X-From-Cache"))
		}
		// Neither failed representation became selectable.
		for _, lang := range []string{"broken", "partial"} {
			req, _ := http.NewRequest("GET", varyTestURL, nil)
			req.Header.Set("Accept-Language", lang)
			if resp, err := h.CachedResponse(cache, req); err != nil {
				t.Fatal(err)
			} else if resp != nil {
				t.Fatalf("half representation for %q reached the cache", lang)
			}
		}
	})

	t.Run("a failed new response does not withdraw the published set", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			if call >= 2 {
				return nil, errors.New("origin unreachable")
			}
			return stringResponse(200, varyHeaders("Accept-Language",
				[2]string{"Cache-Control", "no-cache, stale-if-error"}), "en"), nil
		}}
		tr, _ := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))

		req, _ := http.NewRequest("GET", varyTestURL, nil)
		req.Header.Set("Accept-Language", "fr")
		if _, err := client.Do(req); err == nil {
			t.Fatal("expected transport error")
		}
		// en remains published and a matched request still validates against it.
		res, err := client.Do(reqWithLang("en"))
		if err != nil {
			t.Fatal(err)
		}
		b, _ := io.ReadAll(res.Body)
		res.Body.Close()
		if string(b) != "en" {
			t.Fatalf("published set withdrawn after failure: body=%q", b)
		}
	})

	t.Run("same condition out-of-order EOF keeps the newer body", func(t *testing.T) {
		enteredOldCall := make(chan struct{})
		releaseOldCall := make(chan struct{})
		oldBodyStarted := make(chan struct{})
		newBodyStarted := make(chan struct{})
		releaseOldBody := make(chan struct{})
		releaseNewBody := make(chan struct{})

		rt := &scriptedRT{
			enter: func(call int32) {
				if call == 2 {
					close(enteredOldCall)
					<-releaseOldCall
				}
			},
			handle: func(call int32, req *http.Request) (*http.Response, error) {
				switch call {
				case 1:
					return stringResponse(200, varyHeaders("Accept-Language",
						[2]string{"Cache-Control", "no-cache"}), "en-v1"), nil
				case 2:
					return scriptedResponse(200, varyHeaders("Accept-Language",
						[2]string{"Cache-Control", "no-cache"}),
						&blockingBody{data: []byte("en-v2"), started: oldBodyStarted, release: releaseOldBody}), nil
				default:
					return scriptedResponse(200, varyHeaders("Accept-Language",
						[2]string{"Cache-Control", "no-cache"}),
						&blockingBody{data: []byte("en-v3"), started: newBodyStarted, release: releaseNewBody}), nil
				}
			},
		}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))

		type fetchResult struct {
			body string
			err  error
		}
		oldDone := make(chan fetchResult, 1)
		go func() {
			r, err := client.Do(reqWithLang("en"))
			if err != nil {
				oldDone <- fetchResult{err: err}
				return
			}
			b, err := io.ReadAll(r.Body)
			r.Body.Close()
			oldDone <- fetchResult{body: string(b), err: err}
		}()
		<-enteredOldCall

		newDone := make(chan fetchResult, 1)
		go func() {
			r, err := client.Do(reqWithLang("en"))
			if err != nil {
				newDone <- fetchResult{err: err}
				return
			}
			b, err := io.ReadAll(r.Body)
			r.Body.Close()
			newDone <- fetchResult{body: string(b), err: err}
		}()
		close(releaseOldCall)
		<-oldBodyStarted
		<-newBodyStarted

		// The newer response (call 3) publishes first; the older one finishes last.
		close(releaseNewBody)
		if r := <-newDone; r.err != nil || r.body != "en-v3" {
			t.Fatalf("newer response=%+v", r)
		}
		close(releaseOldBody)
		if r := <-oldDone; r.err != nil || r.body != "en-v2" {
			t.Fatalf("older response=%+v", r)
		}

		enReq := reqWithLang("en")
		enResp, err := h.CachedResponse(cache, enReq)
		if err != nil || enResp == nil {
			t.Fatalf("CachedResponse resp=%v err=%v", enResp, err)
		}
		b, _ := io.ReadAll(enResp.Body)
		enResp.Body.Close()
		if string(b) != "en-v3" {
			t.Fatalf("older late body overwrote the newer one: cached=%q want en-v3", b)
		}
	})

	t.Run("200 with Vary star returns the body but withdraws the url", func(t *testing.T) {
		rt := &scriptedRT{handle: func(call int32, req *http.Request) (*http.Response, error) {
			if call == 1 {
				return stringResponse(200, varyHeaders("Accept-Language",
					[2]string{"Cache-Control", "max-age=3600"}), "en"), nil
			}
			return stringResponse(200, varyHeaders("*",
				[2]string{"Cache-Control", "max-age=3600"}), "star-body"), nil
		}}
		tr, cache := newTestTransport(rt)
		client := tr.Client()
		doGet(t, client, langHeader("en"))

		if body, _ := doGet(t, client, langHeader("fr")); body != "star-body" {
			t.Fatalf("wildcard body=%q", body)
		}
		for _, lang := range []string{"en", "fr"} {
			req, _ := http.NewRequest("GET", varyTestURL, nil)
			req.Header.Set("Accept-Language", lang)
			if resp, err := h.CachedResponse(cache, req); err != nil {
				t.Fatal(err)
			} else if resp != nil {
				t.Fatalf("Vary: * left a reusable representation for %q", lang)
			}
		}
		doGet(t, client, langHeader("en"))
		if rt.originCalls() != 3 {
			t.Fatalf("origin calls=%d want 3", rt.originCalls())
		}
	})
}

func stringsJoin(values []string) string {
	out := ""
	for i, v := range values {
		if i > 0 {
			out += ","
		}
		out += v
	}
	return out
}
