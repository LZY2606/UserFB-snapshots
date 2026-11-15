package ant

import (
	"context"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"

	"github.com/yields/ant/task_support/taskcheck"
)

// refreshScript describes a single scripted response.
type refreshScript struct {
	status  int
	etag    string
	lastmod string
	body    string
	started chan struct{}
	release chan struct{}
}

// refreshRequest records a single observed request.
type refreshRequest struct {
	path string
	inm  string
	ims  string
}

// scriptedServer serves scripted responses per path and records the
// conditional headers of every request it observes.
type scriptedServer struct {
	mu        sync.Mutex
	counts    map[string]int
	records   []refreshRequest
	script    map[string][]refreshScript
	onRequest func(path string, n int)
	srv       *httptest.Server
}

// newScriptedServer returns a scripted server.
func newScriptedServer(t *testing.T, script map[string][]refreshScript) *scriptedServer {
	t.Helper()

	s := &scriptedServer{
		counts: make(map[string]int),
		script: script,
	}
	s.srv = httptest.NewServer(s)
	t.Cleanup(s.srv.Close)
	return s
}

// ServeHTTP implementation.
func (s *scriptedServer) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mu.Lock()
	n := s.counts[r.URL.Path]
	s.counts[r.URL.Path]++
	s.records = append(s.records, refreshRequest{
		path: r.URL.Path,
		inm:  r.Header.Get("If-None-Match"),
		ims:  r.Header.Get("If-Modified-Since"),
	})
	resps := s.script[r.URL.Path]
	var sc refreshScript
	switch {
	case n < len(resps):
		sc = resps[n]
	case len(resps) > 0:
		sc = resps[len(resps)-1]
	default:
		sc.status = http.StatusOK
	}
	hook := s.onRequest
	s.mu.Unlock()

	if hook != nil {
		hook(r.URL.Path, n)
	}
	if sc.started != nil {
		sc.started <- struct{}{}
	}
	if sc.release != nil {
		select {
		case <-sc.release:
		case <-r.Context().Done():
			return
		}
	}

	if sc.etag != "" {
		w.Header().Set("ETag", sc.etag)
	}
	if sc.lastmod != "" {
		w.Header().Set("Last-Modified", sc.lastmod)
	}
	if sc.status != 0 {
		w.WriteHeader(sc.status)
	}
	if sc.body != "" {
		fmt.Fprint(w, sc.body)
	}
}

// URL returns the server URL for the given path.
func (s *scriptedServer) url(path string) string {
	return s.srv.URL + path
}

// Count returns the amount of requests observed for path.
func (s *scriptedServer) count(path string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.counts[path]
}

// Record returns the n-th (0-based) request observed for path.
func (s *scriptedServer) record(path string, n int) (refreshRequest, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	var i int
	for _, rec := range s.records {
		if rec.path == path {
			if i == n {
				return rec, true
			}
			i++
		}
	}
	return refreshRequest{}, false
}

// ScrapeFunc implements a scraper from a function.
type scrapeFunc func(ctx context.Context, p *Page) (URLs, error)

// Scrape implementation.
func (f scrapeFunc) Scrape(ctx context.Context, p *Page) (URLs, error) {
	return f(ctx, p)
}

// noopScrape is a scraper that returns nothing.
var noopScrape = scrapeFunc(func(context.Context, *Page) (URLs, error) {
	return nil, nil
})

// scrapeCounts counts scraper calls per path.
type scrapeCounts struct {
	mu    sync.Mutex
	calls map[string]int
	next  Scraper
}

// Scrape implementation.
func (s *scrapeCounts) Scrape(ctx context.Context, p *Page) (URLs, error) {
	s.mu.Lock()
	s.calls[p.URL.Path]++
	s.mu.Unlock()
	return s.next.Scrape(ctx, p)
}

// Count returns the amount of calls for path.
func (s *scrapeCounts) count(path string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.calls[path]
}

// refreshEngine returns an impolite engine for refresh tests.
func refreshEngine(t *testing.T, s Scraper, workers, concurrency int) *Engine {
	t.Helper()

	eng, err := NewEngine(EngineConfig{
		Scraper:     s,
		Impolite:    true,
		Workers:     workers,
		Concurrency: concurrency,
	})
	if err != nil {
		t.Fatalf("new engine: %s", err)
	}
	return eng
}

// TestRefreshMailbox tests the refresh notification mailbox: inactive
// errors, validation, coalescing, full upgrades and callback handoff.
func TestRefreshMailbox(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var ctx = context.Background()

		// An inactive engine rejects notifications, invalid modes
		// and custom queue or deduper configurations are reported.
		{
			eng := refreshEngine(t, noopScrape, 1, 1)

			err := eng.Refresh(ctx, "http://example.com/", RefreshFull)
			c.Check(errors.Is(err, ErrRefreshInactive),
				"refresh without an active round = %v", err)

			err = eng.Refresh(ctx, "http://example.com/", "bogus")
			c.Check(err != nil && strings.Contains(err.Error(), `"bogus"`),
				"invalid refresh mode = %v", err)

			err = eng.RunRefresh(ctx, "bogus", "http://example.com/")
			c.Check(err != nil && strings.Contains(err.Error(), `"bogus"`),
				"invalid run mode = %v", err)

			custom, err := NewEngine(EngineConfig{
				Scraper: noopScrape,
				Queue:   MemoryQueue(1),
			})
			c.Check(err == nil, "new engine: %v", err)
			err = custom.RunRefresh(ctx, RefreshFull, "http://example.com/")
			c.Check(err != nil && strings.Contains(err.Error(), "queue"),
				"custom queue = %v", err)

			custom, err = NewEngine(EngineConfig{
				Scraper: noopScrape,
				Deduper: DedupeMap(),
			})
			c.Check(err == nil, "new engine: %v", err)
			err = custom.RunRefresh(ctx, RefreshFull, "http://example.com/")
			c.Check(err != nil && strings.Contains(err.Error(), "deduper"),
				"custom deduper = %v", err)
		}

		// Notifications sent while a URL is in-flight coalesce into
		// a single successor fetch that starts only after the
		// callback returned, a full notification upgrades it.
		{
			srv := newScriptedServer(t, map[string][]refreshScript{
				"/a": {
					{status: 200, etag: `"a1"`},
					{status: 200, etag: `"a2"`},
				},
			})

			var eng *Engine
			var calls atomic.Int32
			var callbackReturned atomic.Bool
			var secondAfterCallback atomic.Bool
			var refreshErrs []error

			srv.onRequest = func(path string, n int) {
				if path == "/a" && n == 1 {
					secondAfterCallback.Store(callbackReturned.Load())
				}
			}

			scraper := scrapeFunc(func(ctx context.Context, p *Page) (URLs, error) {
				if calls.Add(1) == 1 {
					err1 := eng.Refresh(ctx, srv.url("/a"), RefreshIncremental)
					err2 := eng.Refresh(ctx, srv.url("/a"), RefreshFull)
					refreshErrs = append(refreshErrs, err1, err2)
					defer callbackReturned.Store(true)
				}
				return nil, nil
			})
			eng = refreshEngine(t, scraper, 1, 1)

			err := eng.RunRefresh(ctx, RefreshIncremental, srv.url("/a"))
			c.Check(err == nil, "run refresh: %v", err)
			c.Check(len(refreshErrs) == 2 && refreshErrs[0] == nil && refreshErrs[1] == nil,
				"in-callback refresh errors = %v", refreshErrs)
			c.Check(srv.count("/a") == 2,
				"coalesced successor fetched once, got %d requests", srv.count("/a"))
			c.Check(calls.Load() == 2,
				"scraper called twice, got %d", calls.Load())
			c.Check(secondAfterCallback.Load(),
				"successor fetch started before the callback returned")

			first, _ := srv.record("/a", 0)
			c.Check(first.inm == "" && first.ims == "",
				"first incremental request sent conditionals %q %q", first.inm, first.ims)

			second, _ := srv.record("/a", 1)
			c.Check(second.inm == "" && second.ims == "",
				"upgraded full successor sent conditionals %q %q", second.inm, second.ims)

			err = eng.Refresh(ctx, srv.url("/a"), RefreshFull)
			c.Check(errors.Is(err, ErrRefreshInactive),
				"refresh after the round = %v", err)
		}

		// Notifications sent while a URL waits for dispatch merge
		// into a single fetch, a full notification upgrades the
		// pending fetch.
		{
			slowStarted := make(chan struct{}, 1)
			slowRelease := make(chan struct{})
			var releaseOnce sync.Once
			defer releaseOnce.Do(func() { close(slowRelease) })

			srv := newScriptedServer(t, map[string][]refreshScript{
				"/slow": {
					{status: 200, started: slowStarted, release: slowRelease},
				},
				"/x": {
					{status: 200, etag: `"x1"`},
					{status: 200, etag: `"x2"`},
				},
				"/y": {
					{status: 200},
				},
			})

			eng := refreshEngine(t, noopScrape, 1, 1)

			err := eng.RunRefresh(ctx, RefreshIncremental, srv.url("/x"))
			c.Check(err == nil, "first round: %v", err)
			c.Check(srv.count("/x") == 1, "first round /x requests = %d", srv.count("/x"))

			runErr := make(chan error, 1)
			go func() {
				runErr <- eng.RunRefresh(ctx, RefreshIncremental, srv.url("/slow"))
			}()

			<-slowStarted

			// The single worker is now blocked by the concurrency
			// limit, /x stays queued while the notifications merge.
			c.Check(eng.Refresh(ctx, srv.url("/y"), RefreshFull) == nil,
				"refresh /y")
			c.Check(eng.Refresh(ctx, srv.url("/x"), RefreshIncremental) == nil,
				"refresh /x incremental")
			c.Check(eng.Refresh(ctx, srv.url("/x"), RefreshFull) == nil,
				"refresh /x full")

			releaseOnce.Do(func() { close(slowRelease) })

			err = <-runErr
			c.Check(err == nil, "second round: %v", err)
			c.Check(srv.count("/x") == 2,
				"merged notifications fetched /x once, got %d", srv.count("/x"))
			c.Check(srv.count("/y") == 1, "/y requests = %d", srv.count("/y"))
			c.Check(srv.count("/slow") == 1, "/slow requests = %d", srv.count("/slow"))

			rec, ok := srv.record("/x", 1)
			c.Check(ok, "second /x request recorded")
			c.Check(rec.inm == "" && rec.ims == "",
				"pending full upgrade sent conditionals %q %q", rec.inm, rec.ims)
		}
	})
}

// TestRefreshRepresentations tests conditional requests, validator
// replacement, 304 handling and representation reuse across rounds.
func TestRefreshRepresentations(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var ctx = context.Background()

		srv := newScriptedServer(t, map[string][]refreshScript{
			"/a": {
				{status: 200, etag: `"a1"`, lastmod: "Mon, 02 Jan 2006 15:04:05 GMT", body: `<a href="/b">b</a>`},
				{status: 304, etag: `"a2"`},
				{status: 200, lastmod: "Tue, 03 Jan 2006 15:04:05 GMT", body: `<a href="/b">b</a>`},
				{status: 200, etag: `"a4"`},
				{status: 200, etag: `"a5"`, body: `<a href="/b">b</a>`},
				{status: 200, etag: `"a6"`, body: `<a href="/b">b</a>`},
			},
			"/b": {
				{status: 200, etag: `"b1"`},
				{status: 304},
				{status: 200, etag: `"b2"`, body: `<a href="/c">c</a>`},
				{status: 500},
				{status: 200, etag: `"b3"`},
			},
			"/c": {
				{status: 200},
			},
		})

		scraper := &scrapeCounts{
			calls: make(map[string]int),
			next: scrapeFunc(func(ctx context.Context, p *Page) (URLs, error) {
				return p.Next("a")
			}),
		}
		eng := refreshEngine(t, scraper, 1, 1)

		record := func(path string, n int) refreshRequest {
			rec, ok := srv.record(path, n)
			c.Check(ok, "request %s #%d was recorded", path, n+1)
			return rec
		}

		// Round 1: first-time incremental requests are full.
		err := eng.RunRefresh(ctx, RefreshIncremental, srv.url("/a"))
		c.Check(err == nil, "round 1: %v", err)
		c.Check(srv.count("/a") == 1 && srv.count("/b") == 1,
			"round 1 requests a=%d b=%d", srv.count("/a"), srv.count("/b"))
		rec := record("/a", 0)
		c.Check(rec.inm == "" && rec.ims == "",
			"first incremental /a sent conditionals %q %q", rec.inm, rec.ims)
		c.Check(scraper.count("/a") == 1 && scraper.count("/b") == 1,
			"round 1 scraper calls a=%d b=%d", scraper.count("/a"), scraper.count("/b"))

		// Round 2: ETag is preferred over Last-Modified, a 304 keeps
		// the old outlinks and re-discovers them without scraping.
		err = eng.RunRefresh(ctx, RefreshIncremental, srv.url("/a"))
		c.Check(err == nil, "round 2: %v", err)
		rec = record("/a", 1)
		c.Check(rec.inm == `"a1"` && rec.ims == "",
			"round 2 /a conditionals %q %q", rec.inm, rec.ims)
		rec = record("/b", 1)
		c.Check(rec.inm == `"b1"` && rec.ims == "",
			"round 2 /b conditionals %q %q", rec.inm, rec.ims)
		c.Check(scraper.count("/a") == 1 && scraper.count("/b") == 1,
			"304 scraped a=%d b=%d", scraper.count("/a"), scraper.count("/b"))

		// Round 3: the 304 updated /a's ETag and kept /b's, a 200
		// without an ETag clears it and keeps Last-Modified.
		err = eng.RunRefresh(ctx, RefreshIncremental, srv.url("/a"))
		c.Check(err == nil, "round 3: %v", err)
		rec = record("/a", 2)
		c.Check(rec.inm == `"a2"` && rec.ims == "",
			"round 3 /a conditionals %q %q", rec.inm, rec.ims)
		rec = record("/b", 2)
		c.Check(rec.inm == `"b1"` && rec.ims == "",
			"round 3 /b conditionals %q %q", rec.inm, rec.ims)
		c.Check(srv.count("/c") == 1, "round 3 /c requests = %d", srv.count("/c"))
		c.Check(scraper.count("/a") == 2 && scraper.count("/b") == 2 && scraper.count("/c") == 1,
			"round 3 scraper calls a=%d b=%d c=%d",
			scraper.count("/a"), scraper.count("/b"), scraper.count("/c"))

		// Round 4: without an ETag the Last-Modified date is sent.
		err = eng.RunRefresh(ctx, RefreshIncremental, srv.url("/a"))
		c.Check(err == nil, "round 4: %v", err)
		rec = record("/a", 3)
		c.Check(rec.inm == "" && rec.ims == "Tue, 03 Jan 2006 15:04:05 GMT",
			"round 4 /a conditionals %q %q", rec.inm, rec.ims)

		// Round 5: a failing status terminates the round with the
		// original error and does not commit the failed candidates.
		err = eng.RunRefresh(ctx, RefreshIncremental, srv.url("/a"))
		var ferr *FetchError
		c.Check(errors.As(err, &ferr) && ferr.Status == 500,
			"round 5 error = %v", err)
		rec = record("/a", 4)
		c.Check(rec.inm == `"a4"`, "round 5 /a conditional %q", rec.inm)
		rec = record("/b", 3)
		c.Check(rec.inm == `"b2"`, "round 5 /b conditional %q", rec.inm)
		c.Check(scraper.count("/b") == 2,
			"failed /b scraped %d times", scraper.count("/b"))

		// Round 6: the failed round committed nothing, the engine
		// re-uses the old representations.
		err = eng.RunRefresh(ctx, RefreshIncremental, srv.url("/a"))
		c.Check(err == nil, "round 6: %v", err)
		rec = record("/a", 5)
		c.Check(rec.inm == `"a4"`,
			"round 6 /a conditional %q, failed round leaked", rec.inm)
		rec = record("/b", 4)
		c.Check(rec.inm == `"b2"`,
			"round 6 /b conditional %q", rec.inm)
		c.Check(scraper.count("/a") == 5 && scraper.count("/b") == 3,
			"round 6 scraper calls a=%d b=%d", scraper.count("/a"), scraper.count("/b"))
	})
}

// TestRefreshDrain tests the round drain boundaries: callback handoff,
// error termination with in-flight wind-down and concurrent rounds.
func TestRefreshDrain(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var ctx = context.Background()

		// Notifications submitted from within a callback are
		// accepted and processed before the round drains.
		{
			srv := newScriptedServer(t, map[string][]refreshScript{
				"/a": {{status: 200, etag: `"a1"`}},
				"/b": {{status: 200, etag: `"b1"`}},
				"/c": {{status: 200}},
			})

			var eng *Engine
			scraper := scrapeFunc(func(ctx context.Context, p *Page) (URLs, error) {
				switch p.URL.Path {
				case "/a":
					return nil, eng.Refresh(ctx, srv.url("/b"), RefreshFull)
				case "/b":
					return nil, eng.Refresh(ctx, srv.url("/c"), RefreshIncremental)
				}
				return nil, nil
			})
			eng = refreshEngine(t, scraper, 1, 1)

			err := eng.RunRefresh(ctx, RefreshFull, srv.url("/a"))
			c.Check(err == nil, "callback handoff round: %v", err)
			c.Check(srv.count("/a") == 1 && srv.count("/b") == 1 && srv.count("/c") == 1,
				"callback handoff requests a=%d b=%d c=%d",
				srv.count("/a"), srv.count("/b"), srv.count("/c"))
		}

		// A scraper that refreshes itself and then fails: the
		// accepted successor is not executed, the original error is
		// returned and in-flight work winds down.
		{
			sentinel := errors.New("scrape boom")
			slowStarted := make(chan struct{}, 1)
			slowCancelled := make(chan struct{}, 1)

			var boomCount atomic.Int32
			mux := http.NewServeMux()
			mux.HandleFunc("/slow", func(w http.ResponseWriter, r *http.Request) {
				slowStarted <- struct{}{}
				select {
				case <-r.Context().Done():
					slowCancelled <- struct{}{}
				case <-ctx.Done():
				}
			})
			mux.HandleFunc("/boom", func(w http.ResponseWriter, r *http.Request) {
				boomCount.Add(1)
				w.Header().Set("ETag", `"boom1"`)
				fmt.Fprint(w, "boom")
			})
			mux.HandleFunc("/ok", func(w http.ResponseWriter, r *http.Request) {
				fmt.Fprint(w, "ok")
			})
			srv := httptest.NewServer(mux)
			t.Cleanup(srv.Close)

			var eng *Engine
			var refreshErrs []error
			scraper := scrapeFunc(func(ctx context.Context, p *Page) (URLs, error) {
				if p.URL.Path == "/boom" {
					<-slowStarted
					refreshErrs = append(refreshErrs,
						eng.Refresh(ctx, srv.URL+"/boom", RefreshFull))
					return nil, sentinel
				}
				return nil, nil
			})
			eng = refreshEngine(t, scraper, 2, 2)

			err := eng.RunRefresh(ctx, RefreshFull, srv.URL+"/slow", srv.URL+"/boom")
			c.Check(errors.Is(err, sentinel),
				"expected the scraper error, got %v", err)
			c.Check(len(refreshErrs) == 1 && refreshErrs[0] == nil,
				"self refresh errors = %v", refreshErrs)
			c.Check(boomCount.Load() == 1,
				"accepted successor executed %d times", boomCount.Load())

			// The in-flight fetch of /slow observed the termination.
			<-slowCancelled

			// The accepted but unexecuted notification is not
			// retained by the next round.
			err = eng.RunRefresh(ctx, RefreshFull, srv.URL+"/ok")
			c.Check(err == nil, "round after failure: %v", err)
			c.Check(boomCount.Load() == 1,
				"notification leaked into the next round, /boom fetched %d times",
				boomCount.Load())
		}

		// Two rounds cannot run concurrently and a canceled round
		// returns the context error.
		{
			started := make(chan struct{}, 1)
			mux := http.NewServeMux()
			mux.HandleFunc("/slow", func(w http.ResponseWriter, r *http.Request) {
				started <- struct{}{}
				<-r.Context().Done()
			})
			mux.HandleFunc("/ok", func(w http.ResponseWriter, r *http.Request) {
				fmt.Fprint(w, "ok")
			})
			srv := httptest.NewServer(mux)
			t.Cleanup(srv.Close)

			eng := refreshEngine(t, noopScrape, 1, 1)

			subctx, cancel := context.WithCancel(ctx)
			runErr := make(chan error, 1)
			go func() {
				runErr <- eng.RunRefresh(subctx, RefreshFull, srv.URL+"/slow")
			}()

			<-started

			err := eng.RunRefresh(ctx, RefreshFull, srv.URL+"/ok")
			c.Check(err != nil && strings.Contains(err.Error(), "already"),
				"concurrent round = %v", err)

			err = eng.Refresh(ctx, srv.URL+"/ok", RefreshFull)
			c.Check(err == nil, "refresh during active round = %v", err)

			cancel()

			err = <-runErr
			c.Check(errors.Is(err, context.Canceled),
				"canceled round = %v", err)

			err = eng.Refresh(ctx, srv.URL+"/ok", RefreshFull)
			c.Check(errors.Is(err, ErrRefreshInactive),
				"refresh after cancelation = %v", err)
		}
	})
}
