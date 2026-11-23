package grawler_test

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"

	"github.com/frobware/grawler"
	"github.com/frobware/grawler/task_support/taskcheck"
)

// serverEvents records per-path hit counts/timings and per-origin and
// global concurrency so scheduling guarantees can be asserted without
// relying on racy sleeps. One recorder may back several servers; the
// global counters then span every origin.
type serverEvents struct {
	mu        sync.Mutex
	counts    map[string]int
	inFlight  map[string]int
	maxOrigin map[string]int
	global    int
	maxGlobal int
	starts    map[string][]time.Time
	finishes  map[string][]time.Time
}

func newServerEvents() *serverEvents {
	return &serverEvents{
		counts:    map[string]int{},
		inFlight:  map[string]int{},
		maxOrigin: map[string]int{},
		starts:    map[string][]time.Time{},
		finishes:  map[string][]time.Time{},
	}
}

func (e *serverEvents) begin(origin, path string) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.counts[path]++
	e.inFlight[origin]++
	if e.inFlight[origin] > e.maxOrigin[origin] {
		e.maxOrigin[origin] = e.inFlight[origin]
	}
	e.global++
	if e.global > e.maxGlobal {
		e.maxGlobal = e.global
	}
	e.starts[path] = append(e.starts[path], time.Now())
}

func (e *serverEvents) end(origin, path string) {
	e.mu.Lock()
	defer e.mu.Unlock()
	e.inFlight[origin]--
	e.global--
	e.finishes[path] = append(e.finishes[path], time.Now())
}

func (e *serverEvents) count(path string) int {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.counts[path]
}

func (e *serverEvents) originInFlight(origin string) int {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.inFlight[origin]
}

func (e *serverEvents) maxConcurrent(origin string) int {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.maxOrigin[origin]
}

func (e *serverEvents) peakGlobal() int {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.maxGlobal
}

func (e *serverEvents) firstStart(path string) time.Time {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.starts[path][0]
}

func (e *serverEvents) firstFinish(path string) time.Time {
	e.mu.Lock()
	defer e.mu.Unlock()
	return e.finishes[path][0]
}

// barrier blocks a handler goroutine until the test releases it.
type barrier struct {
	enter   chan struct{}
	release chan struct{}
	once    sync.Once
}

func newBarrier() *barrier {
	return &barrier{
		enter:   make(chan struct{}, 16),
		release: make(chan struct{}),
	}
}

func (b *barrier) entered() { b.enter <- struct{}{} }
func (b *barrier) wait()    { <-b.release }
func (b *barrier) go_()     { b.once.Do(func() { close(b.release) }) }

func waitEntered(c *taskcheck.C, b *barrier, what string) {
	c.Helper()
	select {
	case <-b.enter:
	case <-time.After(10 * time.Second):
		c.Check(false, "timed out waiting for %s", what)
	}
}

func waitForCount(c *taskcheck.C, ev *serverEvents, path string, want int) {
	c.Helper()
	deadline := time.Now().Add(10 * time.Second)
	for time.Now().Before(deadline) {
		if ev.count(path) >= want {
			return
		}
		time.Sleep(2 * time.Millisecond)
	}
	c.Check(false, "timed out waiting for %d hits on %s, got %d", want, path, ev.count(path))
}

// instrument wraps a handler with start/finish event recording.
func instrument(ev *serverEvents, origin, path string, h http.HandlerFunc) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		ev.begin(origin, path)
		defer ev.end(origin, path)
		h(w, r)
	}
}

func pageMap(pages []*grawler.Page) map[string]*grawler.Page {
	m := make(map[string]*grawler.Page, len(pages))
	for _, page := range pages {
		m[page.URL] = page
	}
	return m
}

// TestHopGates covers the physical GET gates: global and per-origin
// concurrency, the post-response interval (with an independently
// progressing origin), redirect hops obeying the destination origin,
// and relative links resolved against the final response URL.
func TestHopGates(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const gap = 80 * time.Millisecond
		const slack = 25 * time.Millisecond

		ev := newServerEvents()
		b1Barrier := newBarrier()
		jumpTarget := ""

		muxA := http.NewServeMux()
		muxA.HandleFunc("/a1", instrument(ev, "A", "/a1", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<p>a1</p>`)
		}))
		muxA.HandleFunc("/a2", instrument(ev, "A", "/a2", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<p>a2</p>`)
		}))
		muxA.HandleFunc("/jump", instrument(ev, "A", "/jump", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, jumpTarget, http.StatusFound)
		}))

		muxB := http.NewServeMux()
		muxB.HandleFunc("/b1", instrument(ev, "B", "/b1", func(w http.ResponseWriter, r *http.Request) {
			b1Barrier.entered()
			b1Barrier.wait()
			fmt.Fprint(w, `<p>b1</p>`)
		}))
		muxB.HandleFunc("/via", instrument(ev, "B", "/via", func(w http.ResponseWriter, r *http.Request) {
			// Relative link resolved against this final response URL.
			fmt.Fprint(w, `<a href="child">child</a>`)
		}))
		muxB.HandleFunc("/child", instrument(ev, "B", "/child", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<p>child</p>`)
		}))

		serverA := httptest.NewServer(muxA)
		serverB := httptest.NewServer(muxB)
		defer serverA.Close()
		defer serverB.Close()

		jumpTarget = serverB.URL + "/via"

		seeds := []string{
			serverA.URL + "/a1",
			serverA.URL + "/a2",
			serverA.URL + "/jump",
			serverB.URL + "/b1",
		}

		done := make(chan []*grawler.Page, 1)
		go func() {
			pages, err := grawler.CrawlHTTP(seeds, grawler.HTTPOptions{
				MaxWorkers:        2,
				MaxPerOrigin:      1,
				MinOriginInterval: gap,
			}, nil)
			c.Check(err == nil, "CrawlHTTP returned error: %v", err)
			done <- pages
		}()

		// b1 (B) is held behind a barrier while origin A drains its
		// own queue: B must not block A, and only one global slot is
		// occupied by the sleeping B request.
		waitEntered(c, b1Barrier, "server B /b1 barrier")
		waitForCount(c, ev, "/jump", 1)
		c.Check(ev.count("/a1") == 1, "origin A could not progress while B was blocked: /a1=%d", ev.count("/a1"))
		c.Check(ev.count("/a2") == 1, "origin A could not progress while B was blocked: /a2=%d", ev.count("/a2"))
		c.Check(ev.count("/jump") == 1, "A redirect hop not scheduled while B was blocked")
		c.Check(ev.originInFlight("B") == 1, "blocked B request released its origin slot early")
		c.Check(ev.peakGlobal() <= 2, "global concurrency exceeded MaxWorkers: peak=%d", ev.peakGlobal())

		// Consecutive starts at A respect the response interval.
		c.Check(ev.firstStart("/a2").Sub(ev.firstFinish("/a1")) >= gap-slack,
			"/a2 ignored origin interval: delta=%v", ev.firstStart("/a2").Sub(ev.firstFinish("/a1")))
		c.Check(ev.firstStart("/jump").Sub(ev.firstFinish("/a2")) >= gap-slack,
			"/jump ignored origin interval: delta=%v", ev.firstStart("/jump").Sub(ev.firstFinish("/a2")))

		// Release B; the queued redirect hop must wait for B's own
		// post-/b1 interval before starting.
		b1Barrier.go_()
		waitForCount(c, ev, "/via", 1)
		c.Check(ev.firstStart("/via").Sub(ev.firstFinish("/b1")) >= gap-slack,
			"redirect hop ignored destination origin interval: delta=%v",
			ev.firstStart("/via").Sub(ev.firstFinish("/b1")))
		waitForCount(c, ev, "/child", 1)

		var pages []*grawler.Page
		select {
		case pages = <-done:
		case <-time.After(15 * time.Second):
			c.Check(false, "CrawlHTTP did not settle")
			return
		}

		c.Check(ev.maxConcurrent("A") <= 1, "A origin concurrency exceeded MaxPerOrigin: %d", ev.maxConcurrent("A"))
		c.Check(ev.maxConcurrent("B") <= 1, "B origin concurrency exceeded MaxPerOrigin: %d", ev.maxConcurrent("B"))
		c.Check(ev.peakGlobal() <= 2, "global worker limit violated: peak=%d", ev.peakGlobal())

		pm := pageMap(pages)
		// Four seeds plus the relative-link child discovered from the
		// final redirect response; the /via intermediate has no Page.
		c.Check(len(pm) == 5, "expected 5 logical pages, got %d", len(pm))
		_, intermediate := pm[serverB.URL+"/via"]
		c.Check(!intermediate, "redirect intermediate produced a Page")

		jump := pm[serverA.URL+"/jump"]
		c.Check(jump != nil && jump.FetcherError == nil, "jump page missing or failed: %v", jump)
		if jump != nil {
			c.Check(len(jump.Links) == 1 && jump.Links[0] == serverB.URL+"/child",
				"relative link not resolved against final response URL: %v", jump.Links)
		}
		child := pm[serverB.URL+"/child"]
		c.Check(child != nil && child.FetcherError == nil, "relative-link child page missing or failed")
		for _, url := range seeds {
			page, ok := pm[url]
			c.Check(ok, "missing page for seed %q", url)
			if ok {
				c.Check(page.FetcherError == nil, "seed page %q unexpectedly failed: %v", url, page.FetcherError)
			}
		}
	})
}

// TestHopSharing covers convergence: a target discovered through two
// redirect chains and an ordinary link is fetched once, and every
// accepted logical entry receives the shared terminal content under
// its own URL. Queued, in-flight and completed targets all share.
func TestHopSharing(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ev := newServerEvents()
		sharedBarrier := newBarrier()

		mux := http.NewServeMux()
		mux.HandleFunc("/shared", instrument(ev, "O", "/shared", func(w http.ResponseWriter, r *http.Request) {
			sharedBarrier.entered()
			sharedBarrier.wait()
			fmt.Fprint(w, `<p>shared terminal body</p>`)
		}))
		mux.HandleFunc("/r1", instrument(ev, "O", "/r1", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/shared", http.StatusMovedPermanently)
		}))
		mux.HandleFunc("/r2", instrument(ev, "O", "/r2", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/shared", http.StatusTemporaryRedirect)
		}))
		mux.HandleFunc("/doc", instrument(ev, "O", "/doc", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<a href="/shared">shared</a>`)
		}))
		mux.HandleFunc("/finder", instrument(ev, "O", "/finder", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<a href="/early">early</a>`)
		}))
		mux.HandleFunc("/early", instrument(ev, "O", "/early", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<a href="/x">x</a>`)
		}))
		mux.HandleFunc("/x", instrument(ev, "O", "/x", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<p>x</p>`)
		}))

		server := httptest.NewServer(mux)
		defer server.Close()

		seeds := []string{
			server.URL + "/r1",
			server.URL + "/r2",
			server.URL + "/doc",
			server.URL + "/finder",
		}

		done := make(chan []*grawler.Page, 1)
		go func() {
			pages, err := grawler.CrawlHTTP(seeds, grawler.HTTPOptions{
				MaxWorkers:   4,
				MaxPerOrigin: 4,
			}, nil)
			c.Check(err == nil, "CrawlHTTP returned error: %v", err)
			done <- pages
		}()

		waitEntered(c, sharedBarrier, "/shared barrier")
		// While the single physical fetch is in flight, converging
		// entries must not cause a duplicate GET.
		time.Sleep(30 * time.Millisecond)
		c.Check(ev.count("/shared") == 1, "converged in-flight target fetched %d times", ev.count("/shared"))
		sharedBarrier.go_()

		var pages []*grawler.Page
		select {
		case pages = <-done:
		case <-time.After(15 * time.Second):
			c.Check(false, "CrawlHTTP did not settle")
			return
		}

		c.Check(ev.count("/shared") == 1, "converged target fetched %d times total", ev.count("/shared"))
		c.Check(ev.count("/early") == 1, "queued/linked target /early fetched %d times", ev.count("/early"))
		pm := pageMap(pages)

		// Seeds r1, r2, doc, finder; discovered early, x; and /shared
		// itself: it is a redirect-only physical target for r1/r2 but
		// an ordinary discovered link for doc, so it earns its own
		// Page while still being fetched exactly once.
		c.Check(len(pm) == 7, "expected 7 logical pages, got %d", len(pm))
		shared := pm[server.URL+"/shared"]
		c.Check(shared != nil && shared.FetcherError == nil, "discovered /shared page missing or failed")

		// The two redirect chains converge legally: not a cycle, and
		// each keeps its own logical URL with the shared content.
		for _, path := range []string{"/r1", "/r2"} {
			page, ok := pm[server.URL+path]
			c.Check(ok, "missing converged page %q", path)
			if ok {
				c.Check(page.FetcherError == nil, "convergence misclassified as cycle for %q: %v", path, page.FetcherError)
				c.Check(len(page.Links) == 0, "page %q did not receive shared terminal content", path)
			}
		}

		doc := pm[server.URL+"/doc"]
		c.Check(doc != nil && doc.FetcherError == nil, "doc page missing or failed")
		if doc != nil {
			c.Check(len(doc.Links) == 1 && doc.Links[0] == server.URL+"/shared",
				"doc links wrong: %v", doc.Links)
		}
		early := pm[server.URL+"/early"]
		c.Check(early != nil && early.FetcherError == nil, "early page missing or failed")
		if early != nil {
			c.Check(len(early.Links) == 1 && early.Links[0] == server.URL+"/x",
				"early links wrong: %v", early.Links)
		}
		finder := pm[server.URL+"/finder"]
		c.Check(finder != nil && finder.FetcherError == nil, "finder page missing or failed")
		x := pm[server.URL+"/x"]
		c.Check(x != nil && x.FetcherError == nil, "discovered /x page missing or failed")
	})
}

// TestHopCompletion covers terminal behaviour: redirect cycles and
// their predecessors fail with FetcherError, half-read bodies expose
// no links, independent pages keep crawling, the run neither returns
// early nor hangs, and a second call shares no state.
func TestHopCompletion(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ev := newServerEvents()
		gate := newBarrier()

		mux := http.NewServeMux()
		mux.HandleFunc("/cyc1", instrument(ev, "O", "/cyc1", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/cyc2", http.StatusFound)
		}))
		mux.HandleFunc("/cyc2", instrument(ev, "O", "/cyc2", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/cyc1", http.StatusFound)
		}))
		mux.HandleFunc("/pred", instrument(ev, "O", "/pred", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/cyc1", http.StatusFound)
		}))
		mux.HandleFunc("/self", instrument(ev, "O", "/self", func(w http.ResponseWriter, r *http.Request) {
			http.Redirect(w, r, "/self", http.StatusFound)
		}))
		mux.HandleFunc("/badloc", instrument(ev, "O", "/badloc", func(w http.ResponseWriter, r *http.Request) {
			w.WriteHeader(http.StatusFound)
		}))
		mux.HandleFunc("/badscheme", instrument(ev, "O", "/badscheme", func(w http.ResponseWriter, r *http.Request) {
			w.Header().Set("Location", "ftp://example.com/thing")
			w.WriteHeader(http.StatusFound)
		}))
		mux.HandleFunc("/nope", instrument(ev, "O", "/nope", func(w http.ResponseWriter, r *http.Request) {
			http.NotFound(w, r)
		}))
		mux.HandleFunc("/badbody", instrument(ev, "O", "/badbody", func(w http.ResponseWriter, r *http.Request) {
			// Promise more bytes than are ever sent: after the
			// link-bearing prefix is flushed, the body read fails.
			w.Header().Set("Content-Length", "4096")
			w.WriteHeader(http.StatusOK)
			fmt.Fprint(w, `<html><a href="/must-not-expand">leak</a>`)
			if f, ok := w.(http.Flusher); ok {
				f.Flush()
			}
		}))
		mux.HandleFunc("/must-not-expand", instrument(ev, "O", "/must-not-expand", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `should never be fetched`)
		}))
		mux.HandleFunc("/ok1", instrument(ev, "O", "/ok1", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<a href="/ok2">ok2</a>`)
		}))
		mux.HandleFunc("/ok2", instrument(ev, "O", "/ok2", func(w http.ResponseWriter, r *http.Request) {
			fmt.Fprint(w, `<p>ok2</p>`)
		}))
		mux.HandleFunc("/gate", instrument(ev, "O", "/gate", func(w http.ResponseWriter, r *http.Request) {
			gate.entered()
			gate.wait()
			fmt.Fprint(w, `<p>gated</p>`)
		}))

		server := httptest.NewServer(mux)
		defer server.Close()

		seeds := []string{
			server.URL + "/cyc1",
			server.URL + "/pred",
			server.URL + "/self",
			server.URL + "/badloc",
			server.URL + "/badscheme",
			server.URL + "/nope",
			server.URL + "/badbody",
			server.URL + "/ok1",
			server.URL + "/gate",
		}

		done := make(chan []*grawler.Page, 1)
		go func() {
			pages, err := grawler.CrawlHTTP(seeds, grawler.HTTPOptions{
				MaxWorkers:   8,
				MaxPerOrigin: 8,
			}, nil)
			c.Check(err == nil, "CrawlHTTP returned error: %v", err)
			done <- pages
		}()

		waitEntered(c, gate, "/gate barrier")
		// The run must not return while accepted work is still held.
		select {
		case pages := <-done:
			c.Check(false, "CrawlHTTP returned early with %d pages while /gate was held", len(pages))
		case <-time.After(50 * time.Millisecond):
			c.Check(true, "run correctly stayed open while work was in flight")
		}
		gate.go_()

		var pages []*grawler.Page
		select {
		case pages = <-done:
		case <-time.After(15 * time.Second):
			c.Check(false, "CrawlHTTP never settled")
			return
		}

		pm := pageMap(pages)
		failing := []string{
			"/cyc1", "/pred", "/self",
			"/badloc", "/badscheme", "/nope", "/badbody",
		}
		for _, path := range failing {
			page, ok := pm[server.URL+path]
			c.Check(ok, "missing page for failing entry %q", path)
			if ok {
				c.Check(page.FetcherError != nil, "expected FetcherError for %q", path)
			}
		}
		// /cyc2 is only ever a redirect hop with no accepted entry of
		// its own; the cycle error reaches /cyc1 and /pred instead.
		_, cyc2Page := pm[server.URL+"/cyc2"]
		c.Check(!cyc2Page, "redirect-only /cyc2 unexpectedly produced a Page")

		// A half-read body exposes no prefix links and expands nothing.
		if badbody := pm[server.URL+"/badbody"]; badbody != nil {
			c.Check(len(badbody.Links) == 0, "failed body read exposed %d links", len(badbody.Links))
		}
		c.Check(ev.count("/must-not-expand") == 0, "link from a truncated body was expanded")

		// Independent work completes despite the failures.
		ok1 := pm[server.URL+"/ok1"]
		ok2 := pm[server.URL+"/ok2"]
		gated := pm[server.URL+"/gate"]
		c.Check(ok1 != nil && ok1.FetcherError == nil, "independent /ok1 failed")
		c.Check(ok2 != nil && ok2.FetcherError == nil, "discovered /ok2 failed")
		c.Check(gated != nil && gated.FetcherError == nil, "gated page failed")
		if ok1 != nil {
			c.Check(len(ok1.Links) == 1 && ok1.Links[0] == server.URL+"/ok2",
				"healthy page links wrong: %v", ok1.Links)
		}

		// A second, independent call shares no cached state: physical
		// URLs are fetched again and the pages are rebuilt.
		ok1Before := ev.count("/ok1")
		second, err := grawler.CrawlHTTP([]string{server.URL + "/ok1", server.URL + "/nope"},
			grawler.HTTPOptions{MaxWorkers: 2, MaxPerOrigin: 2}, nil)
		c.Check(err == nil, "second CrawlHTTP returned error: %v", err)
		c.Check(ev.count("/ok1") == ok1Before+1, "second call reused state from the first call")
		secondMap := pageMap(second)
		// /ok1 discovers /ok2 again, so the independent run rebuilds
		// three pages from the two seeds.
		c.Check(len(secondMap) == 3, "second run returned %d pages, want 3", len(secondMap))
		if page := secondMap[server.URL+"/ok1"]; page != nil {
			c.Check(page.FetcherError == nil, "second run /ok1 failed: %v", page.FetcherError)
		}
		if page := secondMap[server.URL+"/ok2"]; page != nil {
			c.Check(page.FetcherError == nil, "second run /ok2 failed: %v", page.FetcherError)
		}
		if page := secondMap[server.URL+"/nope"]; page != nil {
			c.Check(page.FetcherError != nil, "second run /nope should fail")
		}
	})
}
