package crawler

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"

	"github.com/s0rg/crawley/internal/client"
)

// fakeResponse is a single controlled server response.
type fakeResponse struct {
	ctype string
	body  string
	code  int
}

// fakeServer is a deterministic in-memory http server facade. It records every
// request and serves preloaded responses keyed by url.
type fakeServer struct {
	mu      sync.Mutex
	routes  map[string]fakeResponse
	gets    map[string]int
	heads   map[string]int
	order   []string
	gates   map[string]<-chan struct{}
	started chan string
}

func newFakeServer() *fakeServer {
	return &fakeServer{
		routes: make(map[string]fakeResponse),
		gets:   make(map[string]int),
		heads:  make(map[string]int),
		gates:  make(map[string]<-chan struct{}),
	}
}

// gate makes a GET for url signal started and then wait for the returned gate
// to close. It lets tests control request interleaving with channels only.
func (f *fakeServer) gate(url string, ch <-chan struct{}) {
	f.mu.Lock()
	defer f.mu.Unlock()

	f.gates[url] = ch
}

func (f *fakeServer) gateRequests(ch chan string) {
	f.mu.Lock()
	defer f.mu.Unlock()

	f.started = ch
}

func (f *fakeServer) add(url, ctype, body string) {
	f.mu.Lock()
	defer f.mu.Unlock()

	f.routes[url] = fakeResponse{ctype: ctype, body: body}
}

func (f *fakeServer) addCode(url string, code int) {
	f.mu.Lock()
	defer f.mu.Unlock()

	f.routes[url] = fakeResponse{code: code}
}

func (f *fakeServer) counts() (gets, heads map[string]int, order []string) {
	f.mu.Lock()
	defer f.mu.Unlock()

	gets = make(map[string]int, len(f.gets))
	heads = make(map[string]int, len(f.heads))

	for k, v := range f.gets {
		gets[k] = v
	}

	for k, v := range f.heads {
		heads[k] = v
	}

	order = append([]string(nil), f.order...)

	return gets, heads, order
}

func (f *fakeServer) Get(_ context.Context, url string) (io.ReadCloser, http.Header, error) {
	f.mu.Lock()
	f.gets[url]++
	f.order = append(f.order, "GET "+url)

	r, ok := f.routes[url]
	gate := f.gates[url]
	started := f.started
	f.mu.Unlock()

	if started != nil {
		select {
		case started <- url:
		default:
		}
	}

	// gate is closed by the test once it observed the desired interleaving
	if gate != nil {
		<-gate
	}

	if !ok {
		return nil, nil, httpErr(http.StatusNotFound)
	}

	hdrs := make(http.Header)

	if r.ctype != "" {
		hdrs.Set(contentType, r.ctype)
	}

	if r.code >= http.StatusBadRequest {
		return io.NopCloser(strings.NewReader(r.body)), hdrs, httpErr(r.code)
	}

	return io.NopCloser(strings.NewReader(r.body)), hdrs, nil
}

func (f *fakeServer) Head(_ context.Context, url string) (http.Header, error) {
	f.mu.Lock()
	f.heads[url]++
	f.order = append(f.order, "HEAD "+url)

	r, ok := f.routes[url]
	f.mu.Unlock()

	if !ok {
		return nil, httpErr(http.StatusNotFound)
	}

	hdrs := make(http.Header)

	if r.ctype != "" {
		hdrs.Set(contentType, r.ctype)
	}

	return hdrs, nil
}

func httpErr(code int) error {
	return client.ErrFromResp(&http.Response{
		StatusCode: code,
		Status:     fmt.Sprintf("%d %s", code, http.StatusText(code)),
	})
}

func runFake(t *testing.T, srv *fakeServer, uri string, opts ...Option) []string {
	t.Helper()

	c := New(opts...)
	c.web = srv

	var (
		mu  sync.Mutex
		got []string
	)

	if err := c.Run(uri, func(s string) {
		mu.Lock()
		got = append(got, s)
		mu.Unlock()
	}); err != nil {
		t.Fatalf("run: %v", err)
	}

	return got
}

// runFakeAsync starts Run in the background and returns a channel with its
// output and an error channel. Used for channel-controlled interleaving.
func runFakeAsync(t *testing.T, srv *fakeServer, uri string, opts ...Option) (<-chan []string, <-chan error) {
	t.Helper()

	c := New(opts...)
	c.web = srv

	out := make(chan []string, 1)
	errs := make(chan error, 1)

	go func() {
		var (
			mu  sync.Mutex
			got []string
		)

		err := c.Run(uri, func(s string) {
			mu.Lock()
			got = append(got, s)
			mu.Unlock()
		})

		mu.Lock()
		out <- got
		mu.Unlock()

		errs <- err
	}()

	return out, errs
}

func hasURL(got []string, u string) bool {
	for _, s := range got {
		if s == u {
			return true
		}
	}

	return false
}

func countURLs(got []string, u string) (n int) {
	for _, s := range got {
		if s == u {
			n++
		}
	}

	return n
}

// TestCSSImportGraph verifies the full dependency collection mode against a
// controlled server: stylesheet expansion, no asset requests, dedupe and
// cycle handling, content-type handling and HEAD behaviour.
func TestCSSImportGraph(t *testing.T) {
	t.Parallel()

	const (
		base       = "http://site.test/"
		styleEntry = "http://site.test/style"
		altLink    = "http://site.test/alt.css"
		icon       = "http://site.test/favicon.ico"
		inlineImp  = "http://site.test/inline-import"
		inlinePic  = "http://site.test/inline.png"
		inlImpPic  = "http://site.test/inline-import.png"
		mainCSS    = "http://site.test/main.css"
		nested     = "http://site.test/nested/deep"
		cyclic     = "http://site.test/cyclic.css"
		loop       = "http://site.test/loop.css"
		noType     = "http://site.test/no-type"
		fromNoType = "http://site.test/from-no-type.css"
		octet      = "http://site.test/octet"
		fromOctet  = "http://site.test/from-octet.css"
		wrongType  = "http://site.test/not-css"
		fromWrong  = "http://site.test/from-wrong.css"
		pic        = "http://site.test/pic.png"
		font       = "http://site.test/font.woff2"
		deepPic    = "http://site.test/nested/deep.png"
		escaped    = "http://site.test/escape%20target.css"
	)

	html := `<html><head>` +
		`<link rel="stylesheet" href="/style">` +
		`<link rel="STYLESHEET preload" href="/style2">` +
		`<link rel="alternate" href="/alt.css">` +
		`<link rel="icon" href="/favicon.ico">` +
		`<style>@import "inline-import"; body{background:url(inline.png)}</style>` +
		`</head><body></body></html>`

	srv := newFakeServer()
	srv.add(base, contentHTML, html)
	srv.add(styleEntry, contentCSS, `@import url("main.css"); @import "escape\20 target.css"; a{background:url(pic.png)}`)
	srv.add("http://site.test/style2", contentCSS, `p{background:url(two.png)}`)
	srv.add(mainCSS, contentCSS, `@import "nested/deep"; @import "not-css"; @import "no-type"; @import "octet"; @font-face{src:url(font.woff2)}`)
	srv.add(inlineImp, contentCSS, `i{background:url(inline-import.png)}`)
	srv.add(nested, "", `@import "../cyclic.css"; n{background:url(deep.png)}`)
	srv.add(cyclic, contentOctet, `@import "loop.css"; c{background:url(cyc.png)}`)
	srv.add(loop, "", `@import url(cyclic.css); l{background:url(loop.png)}`)
	srv.add(noType, "", `@import "from-no-type.css";`)
	srv.add(fromNoType, contentCSS, `u{background:url(no-type.png)}`)
	srv.add(octet, contentOctet, `@import "from-octet.css";`)
	srv.add(fromOctet, contentCSS, `v{background:url(octet.png)}`)
	srv.add(wrongType, contentJS, `@import "from-wrong.css"; a{background:url(wrong.png)}`)
	srv.add(fromWrong, contentCSS, `w{background:url(wrong-imported.png)}`)

	got := runFake(t, srv, base,
		WithScanCSS(true), WithCSSImports(true),
		WithMaxCrawlDepth(-1), WithWorkersCount(2), WithoutHeads(false),
	)
	gets, heads, _ := srv.counts()

	for _, u := range []string{styleEntry, mainCSS, inlineImp, nested, cyclic, loop, noType, octet, fromNoType, fromOctet, wrongType, "http://site.test/style2"} {
		if gets[u] != 1 {
			t.Errorf("GET %s = %d, want 1", u, gets[u])
		}

		if heads[u] != 0 {
			t.Errorf("HEAD %s = %d, want 0", u, heads[u])
		}
	}

	// an explicit non-css content type is fetched but never parsed; none of
	// its references are reported or expanded.
	if gets[fromWrong] != 0 || hasURL(got, fromWrong) || hasURL(got, "http://site.test/wrong.png") {
		t.Errorf("explicit non-css type was expanded: gets=%v out=%v", gets, got)
	}

	// assets are never fetched, no HEAD, no GET
	for _, u := range []string{pic, font, deepPic, inlinePic, inlImpPic, "http://site.test/cyc.png", "http://site.test/loop.png", "http://site.test/wrong.png", altLink, icon} {
		if gets[u] != 0 || heads[u] != 0 {
			t.Errorf("asset %s fetched: GET=%d HEAD=%d", u, gets[u], heads[u])
		}
	}

	for _, u := range []string{pic, font, deepPic, inlinePic, inlImpPic, "http://site.test/two.png"} {
		if !hasURL(got, u) {
			t.Errorf("missing output %s in %v", u, got)
		}
	}

	if !hasURL(got, escaped) {
		t.Errorf("missing escaped import output %s in %v", escaped, got)
	}

	// non-stylesheet links are still reported
	if !hasURL(got, altLink) || !hasURL(got, icon) {
		t.Errorf("missing non-stylesheet links in %v", got)
	}

	// every canonical url is reported at most once
	seen := make(map[string]bool, len(got))
	for _, u := range got {
		if seen[u] {
			t.Errorf("duplicate output %q", u)
		}

		seen[u] = true
	}

	// a cycle merges to one GET per address
	if gets[cyclic] != 1 || gets[loop] != 1 {
		t.Errorf("cycle gets: cyclic=%d loop=%d", gets[cyclic], gets[loop])
	}

	// stylesheet fetches never use HEAD even with the default HEAD mode on;
	// only the ordinary html document keeps its pre-flight HEAD.
	if head, ok := heads[base]; !ok || head != 1 {
		t.Errorf("base document HEAD = %d, want 1 (heads: %v)", head, heads)
	}

	for u, n := range heads {
		if u != base && n != 0 {
			t.Errorf("unexpected HEAD for stylesheet/asset %s: %d", u, n)
		}
	}
}

// TestCSSImportCompatibility checks that the default and legacy css behaviour
// is unchanged when WithCSSImports is off, and that HEAD handling still works.
func TestCSSImportCompatibility(t *testing.T) {
	t.Parallel()

	const (
		base    = "http://legacy.test/"
		mainCSS = "http://legacy.test/main.css"
		child   = "http://legacy.test/child.css"
		pic     = "http://legacy.test/pic.png"
		deep    = "http://legacy.test/deep.png"
	)

	html := `<html><link rel="stylesheet" href="/main.css"></html>`
	css := `@import "child.css"; a{background:url(pic.png)}`

	t.Run("legacy-css-scan", func(t *testing.T) {
		srv := newFakeServer()
		srv.add(base, contentHTML, html)
		srv.add(mainCSS, contentCSS, css)
		srv.add(child, contentCSS, `b{background:url(deep.png)}`)
		srv.add(pic, "image/png", "")

		got := runFake(t, srv, base,
			WithScanCSS(true), WithMaxCrawlDepth(-1), WithWorkersCount(1),
		)
		gets, heads, _ := srv.counts()

		if gets[mainCSS] != 1 || heads[pic] != 1 {
			t.Fatalf("legacy requests: gets=%v heads=%v", gets, heads)
		}

		if gets[child] != 0 || hasURL(got, deep) {
			t.Fatalf("legacy crawled an import: gets=%v out=%v", gets, got)
		}

		if !hasURL(got, pic) {
			t.Fatalf("legacy did not report pic: %v", got)
		}
	})

	t.Run("css-imports-without-css", func(t *testing.T) {
		srv := newFakeServer()
		srv.add(base, contentHTML, html)
		srv.add(mainCSS, contentCSS, css)
		srv.add(pic, "image/png", "")

		got := runFake(t, srv, base,
			WithCSSImports(true), WithMaxCrawlDepth(-1), WithWorkersCount(1),
		)
		gets, _, _ := srv.counts()

		if gets[mainCSS] != 0 || hasURL(got, pic) {
			t.Fatalf("css-imports without -css must be inert: gets=%v out=%v", gets, got)
		}
	})

	t.Run("no-head-in-dependency-mode", func(t *testing.T) {
		srv := newFakeServer()
		srv.add(base, contentHTML, `<html><style>@import "style-no-ext";</style></html>`)
		srv.add("http://legacy.test/style-no-ext", "", `a{background:url(x.png)}`)
		srv.add("http://legacy.test/x.png", "image/png", "")

		got := runFake(t, srv, base,
			WithScanCSS(true), WithCSSImports(true),
			WithMaxCrawlDepth(-1), WithWorkersCount(1), WithoutHeads(true),
		)
		gets, heads, _ := srv.counts()

		if len(heads) != 0 {
			t.Fatalf("unexpected HEAD requests: %v", heads)
		}

		if gets["http://legacy.test/style-no-ext"] != 1 {
			t.Fatalf("extensionless style not fetched: %v", gets)
		}

		if !hasURL(got, "http://legacy.test/x.png") {
			t.Fatalf("missing asset output: %v", got)
		}
	})

	t.Run("asset-then-style", func(t *testing.T) {
		// the address is first reported as a plain resource and later becomes
		// a stylesheet entry: it must still be fetched, but only reported once.
		srv := newFakeServer()
		srv.add(base, contentHTML, `<html>`+
			`<img src="/a.css">`+
			`<style>@import "a.css";</style>`+
			`</html>`)
		srv.add("http://legacy.test/a.css", contentCSS, `a{background:url(inner.png)}`)
		srv.add("http://legacy.test/inner.png", "image/png", "")

		got := runFake(t, srv, base,
			WithScanCSS(true), WithCSSImports(true),
			WithMaxCrawlDepth(-1), WithWorkersCount(1), WithoutHeads(true),
		)
		gets, heads, _ := srv.counts()

		if gets["http://legacy.test/a.css"] != 1 {
			t.Fatalf("type change lost, gets=%v", gets)
		}

		if heads["http://legacy.test/a.css"] != 0 {
			t.Fatalf("unexpected HEAD for style entry: %v", heads)
		}

		if countURLs(got, "http://legacy.test/a.css") != 1 {
			t.Fatalf("style url must be reported once: %v", got)
		}

		if !hasURL(got, "http://legacy.test/inner.png") {
			t.Fatalf("missing imported asset: %v", got)
		}
	})

	t.Run("in-flight-merge-and-cycle", func(t *testing.T) {
		// Two separate stylesheets import the same child and each other. The
		// child GET is held in-flight with a channel while the other branch
		// also discovers it; the whole run must still end with one GET only,
		// and Run must not return before the gated request completes.
		const (
			root    = "http://merge.test/"
			sheetA  = "http://merge.test/a.css"
			sheetB  = "http://merge.test/b.css"
			shared  = "http://merge.test/shared.css"
			sharedP = "http://merge.test/shared.png"
		)

		release := make(chan struct{})
		ready := make(chan string, 4)

		srv := newFakeServer()
		srv.add(root, contentHTML, `<html><style>@import "a.css"; @import "b.css";</style></html>`)
		srv.add(sheetA, contentCSS, `@import "shared.css"; a{background:url(shared.png)}`)
		srv.add(sheetB, contentCSS, `@import "shared.css"; @import "a.css";`)
		srv.add(shared, contentCSS, `@import "a.css"; s{background:url(s2.png)}`)
		srv.gate(shared, release)
		srv.gateRequests(ready)

		out, errs := runFakeAsync(t, srv, root,
			WithScanCSS(true), WithCSSImports(true),
			WithMaxCrawlDepth(-1), WithWorkersCount(3), WithoutHeads(true),
		)

		// wait for the shared sheet to be requested, then release it
		sawShared := false

		for !sawShared {
			select {
			case u := <-ready:
				if u == shared {
					sawShared = true
				}
			case <-t.Context().Done():
				t.Fatal("timed out waiting for the gated request")
			}
		}

		// Run must still be blocked while shared.css is in flight
		select {
		case <-out:
			t.Fatal("Run returned while a style request was in flight")
		case <-t.Context().Done():
			t.Fatal("timed out")
		default:
		}

		close(release)

		var got []string

		select {
		case got = <-out:
		case <-t.Context().Done():
			t.Fatal("timed out waiting for Run")
		}

		if err := <-errs; err != nil {
			t.Fatalf("run: %v", err)
		}

		gets, heads, _ := srv.counts()

		for _, u := range []string{sheetA, sheetB, shared} {
			if gets[u] != 1 {
				t.Errorf("GET %s = %d, want 1 (gets: %v)", u, gets[u], gets)
			}

			if heads[u] != 0 {
				t.Errorf("HEAD %s = %d, want 0", u, heads[u])
			}
		}

		if countURLs(got, shared) != 1 || countURLs(got, sheetA) != 1 {
			t.Fatalf("merged styles must be reported once: %v", got)
		}

		if !hasURL(got, sharedP) {
			t.Fatalf("missing shared asset: %v", got)
		}
	})

	t.Run("multi-level-extensionless", func(t *testing.T) {
		// chains of imports with neither extension nor content type must keep
		// expanding as known stylesheets.
		const (
			root  = "http://chain.test/"
			one   = "http://chain.test/one"
			two   = "http://chain.test/two"
			three = "http://chain.test/three"
			asset = "http://chain.test/three.png"
		)

		srv := newFakeServer()
		srv.add(root, contentHTML, `<html><style>@import "one";</style></html>`)
		srv.add(one, "", `@import url("two");`)
		srv.add(two, contentOctet, `@import "three";`)
		srv.add(three, "", `a{background:url(three.png)}`)

		got := runFake(t, srv, root,
			WithScanCSS(true), WithCSSImports(true),
			WithMaxCrawlDepth(-1), WithWorkersCount(1), WithoutHeads(true),
		)
		gets, heads, _ := srv.counts()

		for _, u := range []string{one, two, three} {
			if gets[u] != 1 || heads[u] != 0 {
				t.Errorf("%s: GET=%d HEAD=%d, want 1/0", u, gets[u], heads[u])
			}
		}

		if gets[asset] != 0 {
			t.Errorf("asset fetched: %v", gets)
		}

		if !hasURL(got, asset) {
			t.Errorf("missing deep asset: %v", got)
		}
	})

	t.Run("ignore-and-depth-policy", func(t *testing.T) {
		// imports inherit the existing ignore and depth policies.
		const (
			root  = "http://policy.test/"
			okCSS = "http://policy.test/ok.css"
			skip  = "http://policy.test/skip/ignored.css"
			deep  = "http://policy.test/skip/deeper.css"
		)

		srv := newFakeServer()
		srv.add(root, contentHTML, `<html><style>@import "ok.css"; @import "skip/ignored.css"; @import "skip/deeper.css";</style></html>`)
		srv.add(okCSS, contentCSS, `a{background:url(ok.png)}`)
		srv.add(skip, contentCSS, `b{background:url(skip.png)}`)
		srv.add(deep, contentCSS, `c{background:url(deep.png)}`)

		got := runFake(t, srv, root,
			WithScanCSS(true), WithCSSImports(true),
			WithMaxCrawlDepth(1), WithWorkersCount(1), WithoutHeads(true),
			WithIgnored([]string{"ignored"}),
		)
		gets, _, _ := srv.counts()

		if gets[okCSS] != 1 {
			t.Errorf("ok import not fetched: %v", gets)
		}

		if gets[skip] != 0 {
			t.Errorf("ignored import fetched: %v", gets)
		}

		// depth 1 from root only reaches /skip/*, not /skip/deeper.css
		if gets[deep] != 0 {
			t.Errorf("depth-limited import: %v", gets)
		}

		if !hasURL(got, "http://policy.test/ok.png") {
			t.Errorf("missing ok asset: %v", got)
		}
	})

	t.Run("headless-off-default-compat", func(t *testing.T) {
		// default mode with HEAD still uses the existing pipeline for links
		srv := newFakeServer()
		srv.add(base, contentHTML, `<html><a href="/page">p</a><img src="/i.png"></html>`)
		srv.add("http://legacy.test/page", contentHTML, `<html><a href="/next">n</a></html>`)
		srv.add("http://legacy.test/next", contentHTML, `<html></html>`)
		srv.add("http://legacy.test/i.png", "image/png", "")

		got := runFake(t, srv, base, WithMaxCrawlDepth(-1), WithWorkersCount(1))
		_, heads, _ := srv.counts()

		if heads["http://legacy.test/page"] != 1 {
			t.Fatalf("legacy HEAD for page missing: %v", heads)
		}

		if !hasURL(got, "http://legacy.test/i.png") {
			t.Fatalf("missing image output: %v", got)
		}
	})
}
