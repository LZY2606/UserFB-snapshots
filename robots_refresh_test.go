// Copyright 2014 Martin Angers and Contributors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package fetchbot

import (
	"errors"
	"io"
	"io/ioutil"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/PuerkitoBio/fetchbot/task_support/taskcheck"
)

// fakeClock is a controllable clock. It replaces the package's clock entry
// points for the duration of a test, so scheduling can be verified by advancing
// simulated time instead of sleeping.
type fakeClock struct {
	mu     sync.Mutex
	now    time.Time
	timers []*fakeTimer
}

type fakeTimer struct {
	at time.Time
	ch chan time.Time
}

func newFakeClock() *fakeClock {
	return &fakeClock{now: time.Date(2026, 1, 1, 0, 0, 0, 0, time.UTC)}
}

func (c *fakeClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *fakeClock) After(d time.Duration) <-chan time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	ch := make(chan time.Time, 1)
	if d <= 0 {
		ch <- c.now
		return ch
	}
	c.timers = append(c.timers, &fakeTimer{at: c.now.Add(d), ch: ch})
	return ch
}

// fireDue releases every registered timer whose deadline has been reached.
func (c *fakeClock) fireDue() {
	c.mu.Lock()
	defer c.mu.Unlock()
	for {
		ix := -1
		for i, tm := range c.timers {
			if !tm.at.After(c.now) && (ix < 0 || tm.at.Before(c.timers[ix].at)) {
				ix = i
			}
		}
		if ix < 0 {
			return
		}
		tm := c.timers[ix]
		c.timers = append(c.timers[:ix], c.timers[ix+1:]...)
		tm.ch <- c.now
	}
}

// step advances the simulated clock by d and fires any timers that became due.
func (c *fakeClock) step(d time.Duration) time.Time {
	c.mu.Lock()
	c.now = c.now.Add(d)
	now := c.now
	c.mu.Unlock()
	c.fireDue()
	return now
}

// stepTo advances the simulated clock to t and fires any timers that became due.
func (c *fakeClock) stepTo(t time.Time) time.Time {
	c.mu.Lock()
	if t.After(c.now) {
		c.now = t
	}
	now := c.now
	c.mu.Unlock()
	c.fireDue()
	return now
}

// hasTimerWithin reports whether a timer due within d (in simulated time)
// from the clock's current time is registered. The long idle-worker timer
// registered by every processed command is filtered out this way.
func (c *fakeClock) hasTimerWithin(d time.Duration) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	limit := c.now.Add(d)
	for _, tm := range c.timers {
		if !tm.at.After(limit) {
			return true
		}
	}
	return false
}

func useFakeClock(t *testing.T) *fakeClock {
	t.Helper()
	c := newFakeClock()
	oldNow, oldAfter := clockNow, clockAfter
	clockNow = c.Now
	clockAfter = c.After
	t.Cleanup(func() {
		clockNow = oldNow
		clockAfter = oldAfter
	})
	return c
}

// recordedRequest captures the simulated time and headers of a Do call.
type recordedRequest struct {
	at     time.Time
	url    string
	header http.Header
}

// robotsSource is a scripted Doer serving robots.txt and page responses for
// several origins. Robots responses are selected by the per-origin hit count.
type robotsSource struct {
	mu     sync.Mutex
	clock  *fakeClock
	robots map[string]func(n int, req *http.Request) (*http.Response, error)
	pages  map[string]func(req *http.Request) (*http.Response, error)

	robotsHits map[string]int
	pageHits   map[string]int
	recorded   []recordedRequest
}

func newRobotsSource(clock *fakeClock) *robotsSource {
	return &robotsSource{
		clock:      clock,
		robots:     make(map[string]func(int, *http.Request) (*http.Response, error)),
		pages:      make(map[string]func(*http.Request) (*http.Response, error)),
		robotsHits: make(map[string]int),
		pageHits:   make(map[string]int),
	}
}

func cloneHeader(h http.Header) http.Header {
	c := make(http.Header, len(h))
	for k, v := range h {
		c[k] = append([]string(nil), v...)
	}
	return c
}

func (s *robotsSource) Do(req *http.Request) (*http.Response, error) {
	key := robotsOriginKey(req.URL)
	at := time.Now()
	if s.clock != nil {
		at = s.clock.Now()
	}
	s.mu.Lock()
	s.recorded = append(s.recorded, recordedRequest{at: at, url: req.URL.String(), header: cloneHeader(req.Header)})
	path := req.URL.Path
	if path == "/robots.txt" {
		n := s.robotsHits[key] + 1
		s.robotsHits[key] = n
		fn := s.robots[key]
		s.mu.Unlock()
		if fn == nil {
			return textResponse(req, 200, ""), nil
		}
		return fn(n, req)
	}
	s.pageHits[key]++
	fn := s.pages[key]
	s.mu.Unlock()
	if fn == nil {
		return textResponse(req, 200, "page"), nil
	}
	return fn(req)
}

func (s *robotsSource) setRobots(key string, fn func(n int, req *http.Request) (*http.Response, error)) {
	s.mu.Lock()
	s.robots[key] = fn
	s.mu.Unlock()
}

func (s *robotsSource) setPages(key string, fn func(*http.Request) (*http.Response, error)) {
	s.mu.Lock()
	s.pages[key] = fn
	s.mu.Unlock()
}

func (s *robotsSource) RobotsHits(key string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.robotsHits[key]
}

func (s *robotsSource) PageHits(key string) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.pageHits[key]
}

// LastRequestTime returns the simulated time of the most recent request to
// the given origin whose path matches path.
func (s *robotsSource) LastRequestTime(key, path string) time.Time {
	s.mu.Lock()
	defer s.mu.Unlock()
	var last time.Time
	for _, rr := range s.recorded {
		if strings.HasPrefix(rr.url, key) && strings.HasPrefix(rr.url, key+path) {
			last = rr.at
		}
	}
	return last
}

// RequestsTo returns every recorded request for the given origin and path.
func (s *robotsSource) RequestsTo(key, path string) []recordedRequest {
	s.mu.Lock()
	defer s.mu.Unlock()
	var out []recordedRequest
	for _, rr := range s.recorded {
		if strings.HasPrefix(rr.url, key) && strings.HasPrefix(rr.url, key+path) {
			out = append(out, rr)
		}
	}
	return out
}

func textResponse(req *http.Request, status int, body string) *http.Response {
	h := make(http.Header)
	h.Set("Content-Type", "text/plain")
	return &http.Response{
		StatusCode: status,
		Header:     h,
		Body:       ioutil.NopCloser(strings.NewReader(body)),
		Request:    req,
	}
}

type failingReader struct{ err error }

func (r failingReader) Read([]byte) (int, error) { return 0, r.err }
func (r failingReader) Close() error             { return nil }

// orderedHandler records every handler call, in arrival order.
type orderedHandler struct {
	mu     sync.Mutex
	urls   []string
	errs   map[string]error
	cmds   map[string]Command
	bodies map[string]string
}

func newOrderedHandler() *orderedHandler {
	return &orderedHandler{
		errs:   make(map[string]error),
		cmds:   make(map[string]Command),
		bodies: make(map[string]string),
	}
}

func (h *orderedHandler) Handle(ctx *Context, res *http.Response, err error) {
	h.mu.Lock()
	defer h.mu.Unlock()
	u := ctx.Cmd.URL().String()
	h.urls = append(h.urls, u)
	h.cmds[u] = ctx.Cmd
	h.errs[u] = err
	if res != nil && res.Body != nil {
		b, _ := ioutil.ReadAll(res.Body)
		h.bodies[u] = string(b)
	}
}

func (h *orderedHandler) count() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.urls)
}

func (h *orderedHandler) ErrFor(u string) error {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.errs[u]
}

func (h *orderedHandler) BodyFor(u string) string {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.bodies[u]
}

func (h *orderedHandler) CmdFor(u string) Command {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.cmds[u]
}

func waitFor(t *testing.T, cond func() bool) bool {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		if cond() {
			return true
		}
		time.Sleep(time.Millisecond)
	}
	return cond()
}

// originState is a consistent copy of a cache entry's state.
type originState struct {
	valid    bool
	allowAll bool
	etag     string
	lastMod  string
	delay    time.Duration
	expires  time.Time
	next     time.Time
}

func snapshotOrigin(o *robotsOrigin) originState {
	o.mu.Lock()
	defer o.mu.Unlock()
	s := originState{
		delay:   o.delay,
		expires: o.expires,
		next:    o.nextAllowed,
	}
	if o.rules != nil {
		s.valid = true
		s.allowAll = o.rules.allowAll
		s.etag = o.rules.etag
		s.lastMod = o.rules.lastModified
	}
	return s
}

func originOf(f *Fetcher, key string) originState {
	return snapshotOrigin(f.getRobotsOrigin(key))
}

func newRefreshFetcher(t *testing.T, src Doer, h Handler, ttl time.Duration) *Fetcher {
	t.Helper()
	f := New(h)
	f.HttpClient = src
	f.CrawlDelay = 0
	f.RobotsCacheTTL = ttl
	f.WorkerIdleTTL = time.Hour
	return f
}

func robotsBody(userAgent, crawlDelay, disallow string) string {
	var b strings.Builder
	if userAgent == "" {
		userAgent = "*"
	}
	b.WriteString("User-agent: " + userAgent + "\n")
	if crawlDelay != "" {
		b.WriteString("Crawl-delay: " + crawlDelay + "\n")
	}
	if disallow != "" {
		b.WriteString("Disallow: " + disallow + "\n")
	}
	return b.String()
}

// probeCmd has its own handler and body provider, used to verify that the
// original Command object reaches it and that the body is only read when the
// page is actually sent.
type probeCmd struct {
	*Cmd
	readerCalls *int32
	calls       chan Command
}

func newProbeCmd(rawurl string, readerCalls *int32) *probeCmd {
	u, err := url.Parse(rawurl)
	if err != nil {
		panic(err)
	}
	return &probeCmd{Cmd: &Cmd{U: u, M: "GET"}, readerCalls: readerCalls, calls: make(chan Command, 1)}
}

func (p *probeCmd) Reader() io.Reader {
	atomic.AddInt32(p.readerCalls, 1)
	return strings.NewReader("probe-body")
}

func (p *probeCmd) Handle(ctx *Context, res *http.Response, err error) {
	if res != nil && res.Body != nil {
		res.Body.Close()
	}
	p.calls <- ctx.Cmd
}

func receiveCommand(t *testing.T, ch chan Command) Command {
	t.Helper()
	select {
	case cmd := <-ch:
		return cmd
	case <-time.After(2 * time.Second):
		return nil
	}
}

func TestRobotsRefreshVersions(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := useFakeClock(t)
		src := newRobotsSource(clock)
		const key = "http://versions.test:80"

		h := newOrderedHandler()
		f := newRefreshFetcher(t, src, h, 5*time.Second)
		q := f.Start()
		defer q.Cancel()

		// Version 1: disallow /private, with both validators.
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			res := textResponse(req, 200, robotsBody("Fetchbot", "", "/private"))
			res.Header.Set("ETag", `"v1"`)
			res.Header.Set("Last-Modified", "lm-v1")
			return res, nil
		})

		if _, err := q.SendStringGet(key+"/ok", key+"/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= 2 }) {
			t.Fatal("timed out waiting for version 1 handling")
		}
		c.Check(src.RobotsHits(key) == 1, "expected one robots request for the first version, got %d", src.RobotsHits(key))
		c.Check(h.BodyFor(key+"/ok") == "page", "allowed page body = %q, want %q", h.BodyFor(key+"/ok"), "page")
		c.Check(h.ErrFor(key+"/private") == ErrDisallowed, "disallowed page error = %v, want ErrDisallowed", h.ErrFor(key+"/private"))
		st := originOf(f, key)
		c.Check(st.valid && !st.allowAll, "version 1 should be a parsed rule version")
		c.Check(st.etag == `"v1"`, "stored ETag = %q, want %q", st.etag, `"v1"`)
		c.Check(st.lastMod == "lm-v1", "stored Last-Modified = %q, want %q", st.lastMod, "lm-v1")

		// Still within the TTL: no refresh, cached version applies.
		clock.stepTo(clock.Now().Add(2 * time.Second))
		if _, err := q.SendStringGet(key + "/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= 3 }) {
			t.Fatal("timed out waiting for cached handling")
		}
		c.Check(src.RobotsHits(key) == 1, "expected no refresh within TTL, robots hits = %d", src.RobotsHits(key))
		c.Check(h.ErrFor(key+"/private") == ErrDisallowed, "cached version error = %v, want ErrDisallowed", h.ErrFor(key+"/private"))

		// Past the TTL, the refresh must be conditional using the stored ETag
		// in preference to Last-Modified. The 304 extends the current version
		// only and its validators are not adopted.
		clock.stepTo(clock.Now().Add(4 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			if n == 2 {
				res := textResponse(req, 304, "")
				res.Header.Set("ETag", `"304-new"`)
				res.Header.Set("Last-Modified", "lm-304-new")
				return res, nil
			}
			return nil, errors.New("unexpected robots request")
		})
		if _, err := q.SendStringGet(key + "/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 2 }) {
			t.Fatal("timed out waiting for conditional refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 4 }) {
			t.Fatal("timed out waiting for 304 handling")
		}
		reqs := src.RequestsTo(key, "/robots.txt")
		c.Check(len(reqs) == 2, "recorded robots requests = %d, want 2", len(reqs))
		if len(reqs) == 2 {
			c.Check(reqs[1].header.Get("If-None-Match") == `"v1"`, "conditional refresh If-None-Match = %q, want %q", reqs[1].header.Get("If-None-Match"), `"v1"`)
			c.Check(reqs[1].header.Get("If-Modified-Since") == "", "ETag should take priority over Last-Modified, got If-Modified-Since %q", reqs[1].header.Get("If-Modified-Since"))
		}
		c.Check(h.ErrFor(key+"/private") == ErrDisallowed, "304 must not allow the disallowed page, error = %v", h.ErrFor(key+"/private"))
		st = originOf(f, key)
		c.Check(st.etag == `"v1"`, "304 must not adopt its ETag, stored = %q", st.etag)
		c.Check(st.lastMod == "lm-v1", "304 must not adopt its Last-Modified, stored = %q", st.lastMod)

		// A 200 with only Last-Modified replaces the version and clears the
		// absent ETag validator. The next refresh uses If-Modified-Since.
		clock.stepTo(clock.Now().Add(6 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			if n == 3 {
				res := textResponse(req, 200, robotsBody("Fetchbot", "", "/private"))
				res.Header.Set("Last-Modified", "lm-v2")
				return res, nil
			}
			if n == 4 {
				return textResponse(req, 304, ""), nil
			}
			return nil, errors.New("unexpected robots request")
		})
		if _, err := q.SendStringGet(key + "/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 3 }) {
			t.Fatal("timed out waiting for second 200 version")
		}
		st = originOf(f, key)
		c.Check(st.etag == "", "ETag absent from the 200 must be cleared, stored = %q", st.etag)
		c.Check(st.lastMod == "lm-v2", "stored Last-Modified = %q, want %q", st.lastMod, "lm-v2")
		if !waitFor(t, func() bool { return h.count() >= 5 }) {
			t.Fatal("timed out waiting for second-version handling")
		}

		clock.stepTo(clock.Now().Add(6 * time.Second))
		if _, err := q.SendStringGet(key + "/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 4 }) {
			t.Fatal("timed out waiting for Last-Modified refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 6 }) {
			t.Fatal("timed out waiting for Last-Modified refresh handling")
		}
		reqs = src.RequestsTo(key, "/robots.txt")
		c.Check(len(reqs) == 4, "recorded robots requests = %d, want 4", len(reqs))
		if len(reqs) == 4 {
			c.Check(reqs[3].header.Get("If-None-Match") == "", "cleared ETag must not be sent, got %q", reqs[3].header.Get("If-None-Match"))
			c.Check(reqs[3].header.Get("If-Modified-Since") == "lm-v2", "If-Modified-Since = %q, want %q", reqs[3].header.Get("If-Modified-Since"), "lm-v2")
		}
		c.Check(h.ErrFor(key+"/private") == ErrDisallowed, "conditional 304 error = %v, want ErrDisallowed", h.ErrFor(key+"/private"))

		// A 404 publishes an allow-all version and clears validators, so the
		// previously disallowed page is now allowed and the next refresh is
		// unconditional.
		clock.stepTo(clock.Now().Add(6 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			if n == 5 {
				return textResponse(req, 404, "not found"), nil
			}
			if n == 6 {
				return textResponse(req, 200, robotsBody("Fetchbot", "", "")), nil
			}
			return nil, errors.New("unexpected robots request")
		})
		if _, err := q.SendStringGet(key + "/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 5 }) {
			t.Fatal("timed out waiting for 404 refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 7 }) {
			t.Fatal("timed out waiting for 404 handling")
		}
		st = originOf(f, key)
		c.Check(st.valid && st.allowAll, "404 must publish an allow-all version, state = %+v", st)
		c.Check(st.etag == "" && st.lastMod == "", "404 must clear validators, etag = %q last-mod = %q", st.etag, st.lastMod)
		c.Check(h.ErrFor(key+"/private") == nil, "404 allow-all should permit the page, error = %v", h.ErrFor(key+"/private"))
		c.Check(h.BodyFor(key+"/private") == "page", "allowed page after 404 body = %q, want %q", h.BodyFor(key+"/private"), "page")

		clock.stepTo(clock.Now().Add(6 * time.Second))
		if _, err := q.SendStringGet(key + "/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 6 }) {
			t.Fatal("timed out waiting for post-404 refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 8 }) {
			t.Fatal("timed out waiting for post-404 page")
		}
		reqs = src.RequestsTo(key, "/robots.txt")
		if len(reqs) == 6 {
			c.Check(reqs[5].header.Get("If-None-Match") == "" && reqs[5].header.Get("If-Modified-Since") == "", "cleared validators should make the refresh unconditional, got %q / %q", reqs[5].header.Get("If-None-Match"), reqs[5].header.Get("If-Modified-Since"))
		}

		// A 410 behaves like a 404: allow-all with cleared validators.
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 410, "gone"), nil
		})
		clock.stepTo(clock.Now().Add(6 * time.Second))
		if _, err := q.SendStringGet(key + "/private"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 7 }) {
			t.Fatal("timed out waiting for 410 refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 9 }) {
			t.Fatal("timed out waiting for 410 handling")
		}
		c.Check(st.valid && st.allowAll, "410 must publish an allow-all version, state = %+v", st)
		c.Check(st.etag == "" && st.lastMod == "", "410 must clear validators, etag = %q last-mod = %q", st.etag, st.lastMod)
		c.Check(h.ErrFor(key+"/private") == nil, "410 allow-all should permit the page, error = %v", h.ErrFor(key+"/private"))
	})
}

func TestRobotsRefreshRecovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := useFakeClock(t)
		src := newRobotsSource(clock)
		const key = "http://recovery.test:80"

		h := newOrderedHandler()
		f := newRefreshFetcher(t, src, h, 5*time.Second)
		q := f.Start()
		defer q.Cancel()

		// First attempt fails at the network level: no usable version ever
		// existed, so the page is not sent and ErrRobotsUnavailable is
		// reported.
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			return nil, errors.New("network down")
		})
		if _, err := q.SendStringGet(key + "/a"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= 1 }) {
			t.Fatal("timed out waiting for unavailable error")
		}
		c.Check(h.ErrFor(key+"/a") == ErrRobotsUnavailable, "first failure error = %v, want ErrRobotsUnavailable", h.ErrFor(key+"/a"))
		c.Check(src.PageHits(key) == 0, "page must not be sent without rules, page hits = %d", src.PageHits(key))
		c.Check(src.RobotsHits(key) == 1, "robots attempts = %d, want 1", src.RobotsHits(key))

		// Before the retry interval expires there is no new robots request,
		// and the command is still only reported as unavailable.
		clock.stepTo(clock.Now().Add(time.Second))
		if _, err := q.SendStringGet(key + "/b"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= 2 }) {
			t.Fatal("timed out waiting for retry-interval error")
		}
		c.Check(src.RobotsHits(key) == 1, "retry must wait for the TTL, robots attempts = %d", src.RobotsHits(key))
		c.Check(src.PageHits(key) == 0, "page must still not be sent, page hits = %d", src.PageHits(key))
		c.Check(h.ErrFor(key+"/b") == ErrRobotsUnavailable, "pre-retry error = %v, want ErrRobotsUnavailable", h.ErrFor(key+"/b"))

		// After the retry interval a valid version is obtained.
		clock.stepTo(clock.Now().Add(5 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			res := textResponse(req, 200, robotsBody("Fetchbot", "", "/x"))
			res.Header.Set("ETag", `"e2"`)
			return res, nil
		})
		if _, err := q.SendStringGet(key + "/ok"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= 3 }) {
			t.Fatal("timed out waiting for recovered version")
		}
		c.Check(src.RobotsHits(key) == 2, "robots attempts after retry = %d, want 2", src.RobotsHits(key))
		c.Check(h.ErrFor(key+"/ok") == nil, "allowed page after recovery error = %v, want nil", h.ErrFor(key+"/ok"))
		c.Check(h.BodyFor(key+"/ok") == "page", "recovered page body = %q, want %q", h.BodyFor(key+"/ok"), "page")
		st := originOf(f, key)
		c.Check(st.valid && !st.allowAll, "recovered version should be parsed rules, state = %+v", st)
		c.Check(st.etag == `"e2"`, "recovered ETag = %q, want %q", st.etag, `"e2"`)

		// A parse error must not publish a new version: the old rules stay in
		// force, including their validators.
		clock.stepTo(clock.Now().Add(5 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, "Disallow: /x\nUser-agent: *\n"), nil
		})
		if _, err := q.SendStringGet(key+"/ok", key+"/x"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 3 }) {
			t.Fatal("timed out waiting for parse-error refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 5 }) {
			t.Fatal("timed out waiting for parse-error handling")
		}
		c.Check(src.PageHits(key) == 2, "page hits after parse error = %d, want 2", src.PageHits(key))
		c.Check(h.ErrFor(key+"/x") == ErrDisallowed, "parse error must keep old rules, /x error = %v", h.ErrFor(key+"/x"))
		st = originOf(f, key)
		c.Check(st.valid && !st.allowAll, "parse error must keep the old version, state = %+v", st)
		c.Check(st.etag == `"e2"`, "parse error must keep the old ETag, got %q", st.etag)

		// A body read error and an unexpected status likewise keep the old
		// version.
		clock.stepTo(clock.Now().Add(5 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			res := textResponse(req, 200, "")
			res.Body = failingReader{err: errors.New("read failed")}
			return res, nil
		})
		if _, err := q.SendStringGet(key + "/x"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 4 }) {
			t.Fatal("timed out waiting for read-error refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 6 }) {
			t.Fatal("timed out waiting for read-error handling")
		}
		c.Check(h.ErrFor(key+"/x") == ErrDisallowed, "read error must keep old rules, /x error = %v", h.ErrFor(key+"/x"))

		clock.stepTo(clock.Now().Add(5 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 500, "server error"), nil
		})
		if _, err := q.SendStringGet(key + "/x"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 5 }) {
			t.Fatal("timed out waiting for status-error refresh")
		}
		if !waitFor(t, func() bool { return h.count() >= 7 }) {
			t.Fatal("timed out waiting for status-error handling")
		}
		c.Check(src.PageHits(key) == 2, "status error must not send the page, page hits = %d", src.PageHits(key))
		c.Check(h.ErrFor(key+"/x") == ErrDisallowed, "status error must keep old rules, /x error = %v", h.ErrFor(key+"/x"))
		st = originOf(f, key)
		c.Check(st.valid && !st.allowAll && st.etag == `"e2"`, "status error changed the published version: %+v", st)

		// A later valid 200 replaces the rules and clears the old validator.
		clock.stepTo(clock.Now().Add(5 * time.Second))
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, robotsBody("Fetchbot", "", "")), nil
		})
		if _, err := q.SendStringGet(key + "/x"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 6 }) {
			t.Fatal("timed out waiting for final recovered version")
		}
		if !waitFor(t, func() bool { return h.count() >= 8 }) {
			t.Fatal("timed out waiting for final-version handling")
		}
		st = originOf(f, key)
		c.Check(st.valid && !st.allowAll, "final version should be parsed rules, state = %+v", st)
		c.Check(st.etag == "", "ETag absent from the final 200 must be cleared, got %q", st.etag)
		c.Check(h.ErrFor(key+"/x") == nil, "final version should allow /x, error = %v", h.ErrFor(key+"/x"))

		// Commands queued after an error are still processed: the queue never
		// loses a command because of a robots failure.
		before := h.count()
		if _, err := q.SendStringGet(key+"/tail1", key+"/tail2"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= before+2 }) {
			t.Fatal("timed out waiting for queued commands after errors")
		}
		c.Check(h.ErrFor(key+"/tail1") == nil && h.ErrFor(key+"/tail2") == nil, "queued tail errors = %v / %v, want nil", h.ErrFor(key+"/tail1"), h.ErrFor(key+"/tail2"))

		// A command with its own handler receives the original object, and its
		// body provider is only consulted when the page is actually sent.
		var readerCalls int32
		denied := newProbeCmd(key+"/x-denied", &readerCalls)
		src.mu.Lock()
		src.robots[key] = func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, robotsBody("Fetchbot", "", "/x-denied")), nil
		}
		src.mu.Unlock()
		clock.stepTo(clock.Now().Add(5 * time.Second))
		if err := q.Send(denied); err != nil {
			t.Fatalf("send: %v", err)
		}
		got := receiveCommand(t, denied.calls)
		c.Check(got == denied, "own handler got %v, want the original *probeCmd", got)
		c.Check(atomic.LoadInt32(&readerCalls) == 0, "body provider called %d times for a denied page, want 0", atomic.LoadInt32(&readerCalls))

		// Cross-worker reuse: the cache outlives idle reclamation. A worker that
		// was reclaimed must reuse the unexpired rules without a new request.
		const reuseKey = "http://reuse.test:80"
		h2 := newOrderedHandler()
		src2 := newRobotsSource(clock)
		f2 := newRefreshFetcher(t, src2, h2, 30*time.Second)
		f2.WorkerIdleTTL = time.Second
		src2.setRobots(reuseKey, func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, robotsBody("Fetchbot", "", "/x")), nil
		})
		q2 := f2.Start()
		defer q2.Cancel()
		if _, err := q2.SendStringGet(reuseKey + "/ok"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h2.count() >= 1 }) {
			t.Fatal("timed out waiting for pre-reclaim command")
		}
		c.Check(src2.RobotsHits(reuseKey) == 1, "pre-reclaim robots hits = %d, want 1", src2.RobotsHits(reuseKey))

		reclaimed := false
		for i := 0; i < 20 && !reclaimed; i++ {
			clock.step(f2.WorkerIdleTTL)
			f2.mu.Lock()
			reclaimed = len(f2.hosts) == 0
			f2.mu.Unlock()
			if !reclaimed {
				time.Sleep(5 * time.Millisecond)
			}
		}
		c.Check(reclaimed, "idle worker was not reclaimed, hosts still registered")
		if _, err := q2.SendStringGet(reuseKey + "/x"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h2.count() >= 2 }) {
			t.Fatal("timed out waiting for post-reclaim command")
		}
		c.Check(src2.RobotsHits(reuseKey) == 1, "rebuilt worker must reuse the cache, robots hits = %d", src2.RobotsHits(reuseKey))
		c.Check(h2.ErrFor(reuseKey+"/x") == ErrDisallowed, "cached rule in rebuilt worker error = %v, want ErrDisallowed", h2.ErrFor(reuseKey+"/x"))

		// Origin isolation: different scheme/host/port origins keep separate
		// caches and rules, while an omitted default port matches the explicit
		// default port.
		h3 := newOrderedHandler()
		src3 := newRobotsSource(clock)
		f3 := newRefreshFetcher(t, src3, h3, 30*time.Second)
		src3.setRobots("http://iso-a.test:80", func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, robotsBody("Fetchbot", "", "/secret")), nil
		})
		src3.setRobots("https://iso-a.test:443", func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, robotsBody("Fetchbot", "", "")), nil
		})
		src3.setRobots("http://iso-b.test:8080", func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, robotsBody("Fetchbot", "", "/secret")), nil
		})
		q3 := f3.Start()
		defer q3.Cancel()
		// Two iso-a commands use the same origin key, one with an omitted
		// default port; they must share one cache and one robots request.
		if _, err := q3.SendStringGet(
			"http://iso-a.test/secret",      // same origin as http://iso-a.test:80
			"https://iso-a.test/secret",     // different scheme, allows
			"http://iso-b.test:8080/secret", // different port, disallows
			"http://iso-b.test/ok",          // default port origin, allows
		); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h3.count() >= 4 }) {
			t.Fatal("timed out waiting for isolation commands")
		}
		c.Check(src3.RobotsHits("http://iso-a.test:80") == 1, "http default-port origin robots hits = %d, want 1", src3.RobotsHits("http://iso-a.test:80"))
		c.Check(src3.RobotsHits("https://iso-a.test:443") == 1, "https origin robots hits = %d, want 1", src3.RobotsHits("https://iso-a.test:443"))
		c.Check(src3.RobotsHits("http://iso-b.test:8080") == 1, "8080 origin robots hits = %d, want 1", src3.RobotsHits("http://iso-b.test:8080"))
		c.Check(src3.RobotsHits("http://iso-b.test:80") == 1, "iso-b default origin robots hits = %d, want 1", src3.RobotsHits("http://iso-b.test:80"))
		c.Check(h3.ErrFor("http://iso-a.test/secret") == ErrDisallowed, "http origin should disallow /secret, error = %v", h3.ErrFor("http://iso-a.test/secret"))
		c.Check(h3.ErrFor("https://iso-a.test/secret") == nil, "https origin should allow /secret, error = %v", h3.ErrFor("https://iso-a.test/secret"))
		c.Check(h3.ErrFor("http://iso-b.test:8080/secret") == ErrDisallowed, "8080 origin should disallow /secret, error = %v", h3.ErrFor("http://iso-b.test:8080/secret"))
		c.Check(h3.ErrFor("http://iso-b.test/ok") == nil, "default-port origin should allow, error = %v", h3.ErrFor("http://iso-b.test/ok"))
		c.Check(robotsOriginKey(mustURL("http://x.test/a")) == robotsOriginKey(mustURL("http://x.test:80/a")), "omitted and explicit default ports must produce the same origin key")
		c.Check(robotsOriginKey(mustURL("HTTP://X.TEST:80/")) == "http://x.test:80", "origin key should lower-case scheme and host, got %q", robotsOriginKey(mustURL("HTTP://X.TEST:80/")))
	})
}

func mustURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}

func TestRobotsRefreshScheduling(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := useFakeClock(t)
		src := newRobotsSource(clock)
		const key = "http://schedule.test:80"

		h := newOrderedHandler()
		f := newRefreshFetcher(t, src, h, 10*time.Second)
		f.CrawlDelay = 2 * time.Second
		q := f.Start()
		defer q.Cancel()

		// Version 1 has no crawl delay, so Fetcher.CrawlDelay applies.
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			return textResponse(req, 200, robotsBody("Fetchbot", "", "")), nil
		})
		start := clock.Now()
		if _, err := q.SendStringGet(key + "/a"); err != nil {
			t.Fatalf("send: %v", err)
		}
		// The robots request is sent immediately; the page waits the crawl
		// delay measured from the robots response.
		if !waitFor(t, func() bool { return clock.hasTimerWithin(2 * time.Second) }) {
			t.Fatal("timed out waiting for the initial crawl delay")
		}
		c.Check(h.count() == 0, "page sent before the robots crawl delay, handler calls = %d", h.count())
		clock.step(2 * time.Second)
		if !waitFor(t, func() bool { return h.count() >= 1 }) {
			t.Fatal("timed out waiting for first page")
		}
		firstRobots := src.LastRequestTime(key, "/robots.txt")
		c.Check(firstRobots.Equal(start), "first robots request at %s, want %s", firstRobots, start)
		firstPage := src.LastRequestTime(key, "/a")
		c.Check(firstPage.Equal(firstRobots.Add(2*time.Second)), "first page at %s, want robots completion + delay %s", firstPage, firstRobots.Add(2*time.Second))

		// The next page waits for the delay from the previous real response.
		if _, err := q.SendStringGet(key + "/b"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return clock.hasTimerWithin(2 * time.Second) }) {
			t.Fatal("timed out waiting for the page crawl delay")
		}
		clock.step(2*time.Second - time.Millisecond)
		time.Sleep(20 * time.Millisecond)
		c.Check(h.count() == 1, "page sent before delay elapsed, handler calls = %d", h.count())
		clock.step(time.Millisecond)
		if !waitFor(t, func() bool { return h.count() >= 2 }) {
			t.Fatal("timed out waiting for delayed page")
		}
		pageB := src.LastRequestTime(key, "/b")
		c.Check(!pageB.Before(firstPage.Add(2*time.Second)), "page /b at %s, want at or after %s", pageB, firstPage.Add(2*time.Second))

		// Refresh with rules that disallow /deny.
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			if n == 2 {
				return textResponse(req, 200, robotsBody("Fetchbot", "", "/deny")), nil
			}
			return nil, errors.New("unexpected robots request")
		})
		clock.stepTo(pageB.Add(10 * time.Second))
		if _, err := q.SendStringGet(key + "/deny"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= 3 }) {
			t.Fatal("timed out waiting for denied page")
		}
		secondRobots := src.LastRequestTime(key, "/robots.txt")
		c.Check(src.RobotsHits(key) == 2, "robots hits after refresh = %d, want 2", src.RobotsHits(key))
		c.Check(h.ErrFor(key+"/deny") == ErrDisallowed, "denied page error = %v, want ErrDisallowed", h.ErrFor(key+"/deny"))
		st := originOf(f, key)
		c.Check(st.next.Equal(secondRobots.Add(f.CrawlDelay)), "next allowed = %s, want %s (robots completion + delay)", st.next, secondRobots.Add(f.CrawlDelay))

		// A denied command immediately after does not shift the established
		// send time.
		if _, err := q.SendStringGet(key + "/deny"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return h.count() >= 4 }) {
			t.Fatal("timed out waiting for second denied page")
		}
		st = originOf(f, key)
		c.Check(st.next.Equal(secondRobots.Add(f.CrawlDelay)), "denied page changed next allowed to %s, want %s", st.next, secondRobots.Add(f.CrawlDelay))

		// The new version carries a crawl delay of 5s, which takes priority
		// over Fetcher.CrawlDelay for the following request. The wait is
		// measured from the robots response completion time.
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			if n == 3 {
				return textResponse(req, 200, robotsBody("Fetchbot", "5", "/deny")), nil
			}
			return nil, errors.New("unexpected robots request")
		})
		clock.stepTo(secondRobots.Add(10 * time.Second))
		if _, err := q.SendStringGet(key + "/c"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 3 }) {
			t.Fatal("timed out waiting for crawl-delay refresh")
		}
		thirdRobots := src.LastRequestTime(key, "/robots.txt")
		c.Check(thirdRobots.Equal(secondRobots.Add(10*time.Second)), "refresh at %s, want %s", thirdRobots, secondRobots.Add(10*time.Second))
		st = originOf(f, key)
		c.Check(st.delay == 5*time.Second, "effective delay after new rules = %s, want 5s", st.delay)
		c.Check(st.next.Equal(thirdRobots.Add(5*time.Second)), "next allowed = %s, want %s", st.next, thirdRobots.Add(5*time.Second))

		// The page must not be sent before the new 5s interval from the robots
		// response.
		if !waitFor(t, func() bool { return clock.hasTimerWithin(5 * time.Second) }) {
			t.Fatal("timed out waiting for the new crawl delay")
		}
		clock.step(5*time.Second - time.Millisecond)
		time.Sleep(20 * time.Millisecond)
		c.Check(h.count() == 4, "page sent before the new crawl delay, handler calls = %d", h.count())
		clock.step(time.Millisecond)
		if !waitFor(t, func() bool { return h.count() >= 5 }) {
			t.Fatal("timed out waiting for page after new crawl delay")
		}
		c.Check(h.ErrFor(key+"/c") == nil, "page /c error = %v, want nil", h.ErrFor(key+"/c"))
		pageC := src.LastRequestTime(key, "/c")
		c.Check(pageC.Equal(thirdRobots.Add(5*time.Second)), "page /c at %s, want robots completion + new delay %s", pageC, thirdRobots.Add(5*time.Second))

		// The old version's established send time is never moved earlier: with
		// a new version whose delay is shorter, an already scheduled later
		// request is not pulled in.
		src.setRobots(key, func(n int, req *http.Request) (*http.Response, error) {
			if n == 4 {
				return textResponse(req, 200, robotsBody("Fetchbot", "1", "/deny")), nil
			}
			return nil, errors.New("unexpected robots request")
		})
		// The TTL is 10s from the third robots response, so the next command
		// past it refreshes. The refresh itself may not be earlier than the
		// old version's established send time.
		clock.stepTo(thirdRobots.Add(10 * time.Second))
		if _, err := q.SendStringGet(key + "/d"); err != nil {
			t.Fatalf("send: %v", err)
		}
		if !waitFor(t, func() bool { return src.RobotsHits(key) >= 4 }) {
			t.Fatal("timed out waiting for shorter-delay refresh")
		}
		fourthRobots := src.LastRequestTime(key, "/robots.txt")
		st = originOf(f, key)
		// The refresh itself must not be earlier than the prior next-allowed.
		c.Check(!fourthRobots.Before(thirdRobots.Add(5*time.Second)), "refresh at %s, earlier than old schedule %s", fourthRobots, thirdRobots.Add(5*time.Second))
		wantNext := fourthRobots.Add(time.Second)
		if oldNext := thirdRobots.Add(5 * time.Second); oldNext.After(wantNext) {
			wantNext = oldNext
		}
		c.Check(!st.next.Before(wantNext), "next allowed = %s, must not be earlier than %s", st.next, wantNext)
	})
}
