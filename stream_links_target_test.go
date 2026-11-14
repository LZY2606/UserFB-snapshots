package gocrawl

import (
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/PuerkitoBio/goquery"

	"github.com/PuerkitoBio/gocrawl/task_support/taskcheck"
)

// notifyBody closes reached once the first `remaining` bytes have been read.
type notifyBody struct {
	io.ReadCloser
	remaining int
	reached   chan struct{}
	once      sync.Once
}

func (b *notifyBody) Read(p []byte) (int, error) {
	n, err := b.ReadCloser.Read(p)
	b.remaining -= n
	if b.remaining <= 0 {
		b.once.Do(func() { close(b.reached) })
	}
	return n, err
}

// chunkedBody forces every Read to return at most chunk bytes, so tags and
// attributes are split across reads.
type chunkedBody struct {
	io.ReadCloser
	chunk int
}

func (b *chunkedBody) Read(p []byte) (int, error) {
	if len(p) > b.chunk {
		p = p[:b.chunk]
	}
	return b.ReadCloser.Read(p)
}

// failAfterBody serves its prefix then fails with err, simulating a broken
// connection mid-response.
type failAfterBody struct {
	prefix []byte
	pos    int
	err    error
}

func (b *failAfterBody) Read(p []byte) (int, error) {
	if b.pos >= len(b.prefix) {
		return 0, b.err
	}
	n := copy(p, b.prefix[b.pos:])
	b.pos += n
	return n, nil
}

func (b *failAfterBody) Close() error { return nil }

type filterEvent struct {
	url       string
	source    string
	isVisited bool
}

// streamExtender records every hook invocation for content assertions.
type streamExtender struct {
	DefaultExtender
	mu sync.Mutex

	visitPaths   []string
	visitedPaths []string
	enqueued     []string
	filters      []filterEvent
	errorKinds   []CrawlErrorKind
	bodies       map[string]string

	// harvest maps a visited path to URLs returned from Visit.
	harvest map[string][]string

	// bodyWrapper optionally rewrites a response body by request path.
	bodyWrapper func(path string, body io.ReadCloser) io.ReadCloser
}

func newStreamExtender() *streamExtender {
	return &streamExtender{bodies: map[string]string{}, harvest: map[string][]string{}}
}

func (e *streamExtender) RequestRobots(ctx *URLContext, robotAgent string) ([]byte, bool) {
	// No robots.txt fetch in the tests.
	return nil, false
}

func (e *streamExtender) Fetch(ctx *URLContext, userAgent string, headRequest bool) (*http.Response, error) {
	res, err := e.DefaultExtender.Fetch(ctx, userAgent, headRequest)
	if err != nil || res == nil || res.Body == nil || e.bodyWrapper == nil {
		return res, err
	}
	if wrapped := e.bodyWrapper(ctx.url.Path, res.Body); wrapped != nil {
		res.Body = wrapped
	}
	return res, nil
}

func (e *streamExtender) Filter(ctx *URLContext, isVisited bool) bool {
	src := ""
	if ctx.SourceURL() != nil {
		src = ctx.SourceURL().String()
	}
	e.mu.Lock()
	e.filters = append(e.filters, filterEvent{ctx.normalizedURL.String(), src, isVisited})
	e.mu.Unlock()
	return !isVisited
}

func (e *streamExtender) Enqueued(ctx *URLContext) {
	e.mu.Lock()
	e.enqueued = append(e.enqueued, ctx.normalizedURL.String())
	e.mu.Unlock()
}

func (e *streamExtender) Visit(ctx *URLContext, res *http.Response, doc *goquery.Document) (interface{}, bool) {
	bd, err := io.ReadAll(res.Body)
	e.mu.Lock()
	defer e.mu.Unlock()
	if err == nil {
		e.bodies[ctx.normalizedURL.Path] = string(bd)
	}
	e.visitPaths = append(e.visitPaths, ctx.normalizedURL.Path)
	if raw := e.harvest[ctx.normalizedURL.Path]; len(raw) > 0 {
		urls := make([]*url.URL, 0, len(raw))
		for _, s := range raw {
			u, perr := url.Parse(s)
			if perr != nil {
				panic(perr)
			}
			urls = append(urls, u)
		}
		return urls, false
	}
	return nil, true
}

func (e *streamExtender) Visited(ctx *URLContext, harvested interface{}) {
	e.mu.Lock()
	e.visitedPaths = append(e.visitedPaths, ctx.normalizedURL.Path)
	e.mu.Unlock()
}

func (e *streamExtender) Error(ce *CrawlError) {
	e.mu.Lock()
	e.errorKinds = append(e.errorKinds, ce.Kind)
	e.mu.Unlock()
}

func (e *streamExtender) snapshot() (visit, visited, enqueued []string, filters []filterEvent, kinds []CrawlErrorKind, bodies map[string]string) {
	e.mu.Lock()
	defer e.mu.Unlock()
	visit = append(visit, e.visitPaths...)
	visited = append(visited, e.visitedPaths...)
	enqueued = append(enqueued, e.enqueued...)
	filters = append(filters, e.filters...)
	kinds = append(kinds, e.errorKinds...)
	bodies = make(map[string]string, len(e.bodies))
	for k, v := range e.bodies {
		bodies[k] = v
	}
	return
}

func streamTestOptions(e *streamExtender, enabled bool) *Options {
	opts := NewOptions(e)
	opts.CrawlDelay = 0
	opts.WorkerIdleTTL = 0
	opts.SameHostOnly = false
	opts.URLNormalizationFlags = 0
	opts.LogFlags = 0
	opts.StreamLinks = enabled
	return opts
}

func waitSignal(t *testing.T, ch <-chan struct{}, msg string) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(5 * time.Second):
		t.Fatal(msg)
	}
}

func countString(list []string, want string) int {
	n := 0
	for _, v := range list {
		if v == want {
			n++
		}
	}
	return n
}

// newLeafServer serves a simple article for every non-robots path and counts
// the GET requests it receives. leafReq is closed on the first article GET.
func newLeafServer(t *testing.T, leafHits *int32, leafReq chan struct{}) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/robots.txt" {
			io.WriteString(w, "User-agent: *\nAllow: /\n")
			return
		}
		if atomic.AddInt32(leafHits, 1) == 1 {
			close(leafReq)
		}
		io.WriteString(w, "<html>article at "+r.URL.Path+"</html>")
	}))
}

// newBlockedDirectoryServer flushes the prefix, blocks on release, then
// streams the tail.
func newBlockedDirectoryServer(t *testing.T, prefix, tail string, release chan struct{}) *httptest.Server {
	t.Helper()
	return httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/robots.txt" {
			io.WriteString(w, "User-agent: *\nAllow: /\n")
			return
		}
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, prefix)
		w.(http.Flusher).Flush()
		<-release
		io.WriteString(w, tail)
	}))
}

// TestStreamEarly proves that with StreamLinks enabled, the article GET is
// issued while the directory body is still open, before the parent Visit
// hook runs; and that disabling the option restores the legacy EOF-bound
// ordering.
func TestStreamEarly(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Phase 1: streaming enabled.
		var leafHits int32
		leafReq := make(chan struct{})
		leaf := newLeafServer(t, &leafHits, leafReq)
		defer leaf.Close()

		release := make(chan struct{})
		prefixRead := make(chan struct{})
		prefix := `<html><head><base href="` + leaf.URL + `/"></head><body><a href="item">article</a>`
		tail := `<!-- tail marker -->TAIL_MARKER</body></html>`
		parent := newBlockedDirectoryServer(t, prefix, tail, release)
		defer parent.Close()

		ext := newStreamExtender()
		prefixLen := len(prefix)
		ext.bodyWrapper = func(path string, body io.ReadCloser) io.ReadCloser {
			if path == "/directory" {
				return &notifyBody{ReadCloser: body, remaining: prefixLen, reached: prefixRead}
			}
			return body
		}
		crawler := NewCrawlerWithOptions(streamTestOptions(ext, true))

		done := make(chan error, 1)
		go func() { done <- crawler.Run(parent.URL + "/directory") }()

		waitSignal(t, prefixRead, "directory prefix was not read")
		waitSignal(t, leafReq, "article GET did not happen while the directory body was open")

		visits, _, _, _, _, _ := ext.snapshot()
		c.Check(atomic.LoadInt32(&leafHits) == 1, "want exactly one article GET before parent EOF, got %d", leafHits)
		c.Check(countString(visits, "/directory") == 0, "parent Visit ran before the body completed")

		close(release)
		select {
		case err := <-done:
			c.Check(err == nil, "crawl returned error: %v", err)
		case <-time.After(5 * time.Second):
			t.Fatal("crawl did not settle after releasing the directory tail")
		}

		visits, visited, enqueued, filters, _, bodies := ext.snapshot()
		c.Check(countString(visits, "/directory") == 1, "want one parent Visit, got %v", visits)
		c.Check(countString(visited, "/directory") == 1, "want one parent Visited call, got %v", visited)
		c.Check(strings.Contains(bodies["/directory"], "TAIL_MARKER"), "Visit body was not the complete re-readable document")
		c.Check(countString(enqueued, leaf.URL+"/item") == 1, "streamed article missing from enqueued: %v", enqueued)
		c.Check(len(filters) > 0, "streamed article should go through Filter")
		articleSource := ""
		for _, f := range filters {
			if f.url == leaf.URL+"/item" {
				articleSource = f.source
			}
		}
		c.Check(articleSource == parent.URL+"/directory", "article source must be the parent page, got %q", articleSource)

		// Phase 2: option disabled, legacy behaviour.
		var offLeafHits int32
		offLeafReq := make(chan struct{})
		offLeaf := newLeafServer(t, &offLeafHits, offLeafReq)
		defer offLeaf.Close()

		offRelease := make(chan struct{})
		offPrefixRead := make(chan struct{})
		offPrefix := `<html><body><a href="` + offLeaf.URL + `/item">article</a>`
		offParent := newBlockedDirectoryServer(t, offPrefix, `</body></html>`, offRelease)
		defer offParent.Close()

		offExt := newStreamExtender()
		offExt.bodyWrapper = func(path string, body io.ReadCloser) io.ReadCloser {
			if path == "/directory" {
				return &notifyBody{ReadCloser: body, remaining: len(offPrefix), reached: offPrefixRead}
			}
			return body
		}
		offCrawler := NewCrawlerWithOptions(streamTestOptions(offExt, false))
		offDone := make(chan error, 1)
		go func() { offDone <- offCrawler.Run(offParent.URL + "/directory") }()

		waitSignal(t, offPrefixRead, "disabled-mode prefix was not read")
		select {
		case <-offLeafReq:
			c.Check(false, "article GET happened early with StreamLinks disabled")
		case <-time.After(300 * time.Millisecond):
			c.Check(atomic.LoadInt32(&offLeafHits) == 0, "no article GET may be issued before parent EOF when disabled")
		}
		close(offRelease)
		select {
		case err := <-offDone:
			c.Check(err == nil, "disabled-mode crawl returned error: %v", err)
		case <-time.After(5 * time.Second):
			t.Fatal("disabled-mode crawl did not settle")
		}
		offVisits, _, _, _, _, _ := offExt.snapshot()
		c.Check(atomic.LoadInt32(&offLeafHits) == 1, "article still fetched once after parent EOF when disabled, got %d", offLeafHits)
		c.Check(countString(offVisits, "/directory") == 1, "parent Visit missing in disabled mode: %v", offVisits)
	})
}

// TestStreamTokens covers the token-level extraction rules: head base URL,
// tags split across reads, entities, fake links in comments and raw text
// elements, duplicate discovery, and empty/fragment hrefs.
func TestStreamTokens(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		leaf := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			io.WriteString(w, "<html>leaf</html>")
		}))
		defer leaf.Close()

		doc := `<html><head><base href="` + leaf.URL + `/base/">` +
			`<!-- <a href="fake-comment.html">comment</a> -->` +
			`<script>var s = '<a href="fake-script.html">';</script>` +
			`<style>.x{background:url('<a href="fake-style.html">')}</style>` +
			`<a href="head-link.html">head-only</a>` +
			`</head><body>` +
			`<a href="good1.html">one</a>` +
			`<a hr` + `ef="good2.html?x=1&amp;y=2">two</a>` +
			`<a href="good1.html">duplicate</a>` +
			`<a href="">empty</a>` +
			`<a href="#section">fragment</a>` +
			`</body></html>`

		parent := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			io.WriteString(w, doc)
		}))
		defer parent.Close()

		ext := newStreamExtender()
		ext.bodyWrapper = func(path string, body io.ReadCloser) io.ReadCloser {
			// Two-byte reads split every tag and attribute across chunks.
			return &chunkedBody{ReadCloser: body, chunk: 2}
		}
		crawler := NewCrawlerWithOptions(streamTestOptions(ext, true))
		err := crawler.Run(parent.URL + "/dir")
		c.Check(err == nil, "crawl returned error: %v", err)

		_, _, enqueued, filters, _, _ := ext.snapshot()
		joined := strings.Join(enqueued, ",")
		for _, fake := range []string{"fake-comment.html", "fake-script.html", "fake-style.html"} {
			c.Check(!strings.Contains(joined, fake), "fake link %s was extracted: %s", fake, joined)
		}
		c.Check(strings.Contains(joined, leaf.URL+"/base/good1.html"), "head-base relative link not enqueued: %s", joined)
		c.Check(strings.Contains(joined, leaf.URL+"/base/head-link.html"), "head-section link not released at </head> with the head base: %s", joined)
		c.Check(strings.Contains(joined, leaf.URL+"/base/good2.html?x=1&y=2"), "entity-escaped href not resolved correctly: %s", joined)
		c.Check(!strings.Contains(joined, "#section"), "fragment link was enqueued: %s", joined)
		c.Check(!strings.Contains(joined, ",,"), "empty href produced an empty enqueue entry: %s", joined)

		good1 := leaf.URL + "/base/good1.html"
		c.Check(countString(enqueued, good1) == 1, "duplicate discovery enqueued twice: %v", enqueued)
		good1Filters := 0
		good1SecondVisited := false
		good1Source := ""
		for _, f := range filters {
			if f.url == good1 {
				good1Filters++
				if good1Filters == 2 {
					good1SecondVisited = f.isVisited
				}
				good1Source = f.source
			}
		}
		c.Check(good1Filters == 2, "duplicate discovery must pass Filter twice, got %d", good1Filters)
		c.Check(good1SecondVisited, "second discovery must be reported as already visited")
		c.Check(good1Source == parent.URL+"/dir", "streamed link source must be the parent page, got %q", good1Source)
	})
}

// TestStreamLifecycle covers read failures, the final Visit-harvested links,
// the absence of a whole-document re-scan, and MaxVisits accounting.
func TestStreamLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Phase 1: read failure after a link was already streamed.
		var leafHits int32
		leafReq := make(chan struct{})
		leaf := newLeafServer(t, &leafHits, leafReq)
		defer leaf.Close()

		parent := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			io.WriteString(w, "<html><body>")
		}))
		defer parent.Close()

		readErr := errors.New("simulated truncated body")
		ext := newStreamExtender()
		streamedAnchor := `<html><body><a href="` + leaf.URL + `/item">article</a><p>rest of page`
		ext.bodyWrapper = func(path string, body io.ReadCloser) io.ReadCloser {
			if path == "/dir" {
				return &failAfterBody{prefix: []byte(streamedAnchor), err: readErr}
			}
			return body
		}
		crawler := NewCrawlerWithOptions(streamTestOptions(ext, true))
		done := make(chan error, 1)
		go func() { done <- crawler.Run(parent.URL + "/dir") }()
		waitSignal(t, leafReq, "streamed article was not processed after the read failure")
		select {
		case err := <-done:
			c.Check(err == nil, "parent request must still settle, got error %v", err)
		case <-time.After(5 * time.Second):
			t.Fatal("crawl stuck after the read failure")
		}

		visits, visited, _, _, kinds, _ := ext.snapshot()
		readBodyErrors := 0
		for _, k := range kinds {
			if k == CekReadBody {
				readBodyErrors++
			}
		}
		c.Check(readBodyErrors == 1, "want exactly one CekReadBody report, got %d", readBodyErrors)
		c.Check(countString(visits, "/dir") == 0, "truncated page must not produce a Visit: %v", visits)
		c.Check(countString(visited, "/dir") == 0, "truncated page must not produce a Visited call: %v", visited)
		c.Check(atomic.LoadInt32(&leafHits) == 1, "already-published link must still be processed, got %d hits", leafHits)

		// Phase 2: Visit-harvested links keep flowing, streamed links are not
		// re-scanned at the end.
		harvestServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			if r.URL.Path == "/dir" {
				io.WriteString(w, "<html><body><a href=\"streamed.html\">s</a></body></html>")
				return
			}
			io.WriteString(w, "<html>leaf</html>")
		}))
		defer harvestServer.Close()

		harvestExt := newStreamExtender()
		harvestExt.harvest["/dir"] = []string{harvestServer.URL + "/extra.html"}
		harvestCrawler := NewCrawlerWithOptions(streamTestOptions(harvestExt, true))
		err := harvestCrawler.Run(harvestServer.URL + "/dir")
		c.Check(err == nil, "harvest crawl returned error: %v", err)

		_, _, harvestEnqueued, harvestFilters, _, _ := harvestExt.snapshot()
		harvestJoined := strings.Join(harvestEnqueued, ",")
		c.Check(strings.Contains(harvestJoined, harvestServer.URL+"/streamed.html"), "streamed link missing: %s", harvestJoined)
		c.Check(strings.Contains(harvestJoined, harvestServer.URL+"/extra.html"), "Visit-harvested link missing: %s", harvestJoined)
		streamedFilterCalls := 0
		extraSource := ""
		for _, f := range harvestFilters {
			if f.url == harvestServer.URL+"/streamed.html" {
				streamedFilterCalls++
			}
			if f.url == harvestServer.URL+"/extra.html" {
				extraSource = f.source
			}
		}
		c.Check(streamedFilterCalls == 1, "streamed link re-scanned at end of visit, Filter calls = %d", streamedFilterCalls)
		c.Check(extraSource == harvestServer.URL+"/dir", "Visit-harvested link source must be the parent page, got %q", extraSource)

		// Phase 3: MaxVisits counts successful visits, not link discoveries.
		var maxLeafHits int32
		maxLeafReq := make(chan struct{})
		maxLeaf := newLeafServer(t, &maxLeafHits, maxLeafReq)
		defer maxLeaf.Close()

		maxRelease := make(chan struct{})
		maxPrefixRead := make(chan struct{})
		maxPrefix := `<html><body><a href="` + maxLeaf.URL + `/item">article</a>`
		maxParent := newBlockedDirectoryServer(t, maxPrefix, `</body></html>`, maxRelease)
		defer maxParent.Close()

		maxExt := newStreamExtender()
		maxExt.bodyWrapper = func(path string, body io.ReadCloser) io.ReadCloser {
			if path == "/directory" {
				return &notifyBody{ReadCloser: body, remaining: len(maxPrefix), reached: maxPrefixRead}
			}
			return body
		}
		maxOpts := streamTestOptions(maxExt, true)
		maxOpts.MaxVisits = 1
		maxCrawler := NewCrawlerWithOptions(maxOpts)
		maxDone := make(chan error, 1)
		go func() { maxDone <- maxCrawler.Run(maxParent.URL + "/directory") }()
		waitSignal(t, maxPrefixRead, "max-visits prefix was not read")
		waitSignal(t, maxLeafReq, "max-visits discovery was not enqueued")
		close(maxRelease)
		select {
		case err := <-maxDone:
			c.Check(err == ErrMaxVisits, "want ErrMaxVisits after the parent visit, got %v", err)
		case <-time.After(5 * time.Second):
			t.Fatal("max-visits crawl did not settle")
		}
		_, _, maxEnqueued, _, _, _ := maxExt.snapshot()
		c.Check(countString(maxEnqueued, maxLeaf.URL+"/item") == 1, "discovery messages must still enqueue under MaxVisits: %v", maxEnqueued)
	})
}
