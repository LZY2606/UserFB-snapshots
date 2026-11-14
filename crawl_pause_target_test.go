package gocrawl

import (
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/PuerkitoBio/goquery"

	"github.com/PuerkitoBio/gocrawl/task_support/taskcheck"
)

// The pause tests deliberately avoid the network: the Fetch hook is fully
// controlled and serves deterministic responses, so that phases, bodies,
// callbacks and their ordering can be recorded.

// pauseFixture is a controlled Extender. Every hook is recorded in order, and
// the Fetch hook can block on per-URL gates so that a pause can be triggered
// in the middle of a HEAD, a GET or a robots.txt request.
type pauseFixture struct {
	mu sync.Mutex

	// calls is the ordered trace of hooks and phase boundaries.
	calls []string
	// fetches counts Fetch calls per method+path.
	fetches map[string]int
	visits  map[string]int
	// fetchTimes records when each Fetch call happened.
	fetchTimes map[string][]time.Time
	// bodiesClosed counts how many response bodies the crawler closed.
	bodiesClosed int

	// visitState and visitSource capture the URLContext identity at Visit.
	visitState  map[string]interface{}
	visitSource map[string]string

	// blocked maps "METHOD path" to the channel a Fetch call blocks on.
	blocked map[string]chan struct{}
	// entered signals that a blocked Fetch was entered.
	entered map[string]chan struct{}
	// blockArmed reports whether the block may still fire.
	blockArmed map[string]bool

	// pages maps a path to the body served for it.
	pages map[string]string
	// redirects maps a path to a redirect target.
	redirects map[string]string
	// robots maps a host to its robots.txt content.
	robots map[string]string
	// requestRobotsCache makes RequestRobots answer from memory.
	requestRobotsCache bool

	starts int
	ends   int

	// EnqueueChan is wired by the crawler through reflection.
	EnqueueChan chan<- interface{}
}

func newPauseFixture() *pauseFixture {
	return &pauseFixture{
		fetches:     map[string]int{},
		visits:      map[string]int{},
		fetchTimes:  map[string][]time.Time{},
		visitState:  map[string]interface{}{},
		visitSource: map[string]string{},
		blocked:     map[string]chan struct{}{},
		entered:     map[string]chan struct{}{},
		blockArmed:  map[string]bool{},
		pages:       map[string]string{},
		redirects:   map[string]string{},
		robots:      map[string]string{},
	}
}

// trackedBody wraps a body so that the fixture knows when it gets closed.
type trackedBody struct {
	io.Reader
	f     *pauseFixture
	label string
}

func (b *trackedBody) Close() error {
	b.f.mu.Lock()
	b.f.bodiesClosed++
	b.f.calls = append(b.f.calls, "body-close:"+b.label)
	b.f.mu.Unlock()
	return nil
}

func (f *pauseFixture) appendCall(s string) {
	f.calls = append(f.calls, s)
}

func (f *pauseFixture) record(s string) {
	f.mu.Lock()
	f.appendCall(s)
	f.mu.Unlock()
}

func (f *pauseFixture) snapshotCalls() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.calls...)
}

// countCall counts calls equal to s or with an s: suffix.
func (f *pauseFixture) countCall(s string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	n := 0
	for _, c := range f.calls {
		if c == s || strings.HasPrefix(c, s+":") {
			n++
		}
	}
	return n
}

func (f *pauseFixture) fetchCount(key string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.fetches[key]
}

func (f *pauseFixture) visitCount(path string) int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.visits[path]
}

func (f *pauseFixture) startsCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.starts
}

func (f *pauseFixture) endsCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.ends
}

func (f *pauseFixture) lastFetchTimes(key string) []time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]time.Time(nil), f.fetchTimes[key]...)
}

func (f *pauseFixture) blockOn(key string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	g := make(chan struct{})
	e := make(chan struct{})
	f.blocked[key] = g
	f.entered[key] = e
	f.blockArmed[key] = true
}

func (f *pauseFixture) release(key string) {
	f.mu.Lock()
	ch := f.blocked[key]
	delete(f.blocked, key)
	f.mu.Unlock()
	if ch != nil {
		close(ch)
	}
}

func (f *pauseFixture) waitEntered(t *testing.T, key string) {
	t.Helper()
	f.mu.Lock()
	ch := f.entered[key]
	f.mu.Unlock()
	if ch == nil {
		t.Fatalf("no block registered for %s", key)
	}
	select {
	case <-ch:
	case <-time.After(8 * time.Second):
		t.Fatalf("timed out waiting for %s to be entered", key)
	}
}

func (f *pauseFixture) Start(seeds interface{}) interface{} {
	f.mu.Lock()
	f.starts++
	f.appendCall("Start")
	f.mu.Unlock()
	return seeds
}

func (f *pauseFixture) End(err error) {
	f.mu.Lock()
	f.ends++
	f.appendCall(fmt.Sprintf("End:%v", err))
	f.mu.Unlock()
}

func (f *pauseFixture) Error(err *CrawlError) {
	f.record("Error:" + err.Error())
}

func (f *pauseFixture) Log(LogFlags, LogFlags, string) {}

func (f *pauseFixture) ComputeDelay(host string, di *DelayInfo, lastFetch *FetchInfo) time.Duration {
	f.mu.Lock()
	defer f.mu.Unlock()
	last := "nil"
	if lastFetch != nil {
		last = lastFetch.Ctx.URL().Path
	}
	f.appendCall("ComputeDelay:" + last)
	if di.RobotsDelay > 0 {
		return di.RobotsDelay
	}
	return di.OptsDelay
}

func (f *pauseFixture) Fetch(ctx *URLContext, agent string, headRequest bool) (*http.Response, error) {
	method := "GET"
	if headRequest {
		method = "HEAD"
	}
	key := method + " " + ctx.url.Path

	f.mu.Lock()
	f.fetches[key]++
	f.fetchTimes[key] = append(f.fetchTimes[key], time.Now())
	f.appendCall("Fetch:" + key)
	block := f.blocked[key]
	armed := f.blockArmed[key]
	if armed {
		f.blockArmed[key] = false
	}
	entered := f.entered[key]
	target := f.redirects[ctx.url.Path]
	f.mu.Unlock()

	if block != nil && armed {
		select {
		case <-entered:
		default:
			close(entered)
		}
		<-block
	}

	// A redirect is modelled as the ErrEnqueueRedirect the real extender
	// produces, so that the redirect URL is enqueued through EnqueueChan.
	if target != "" {
		return nil, &url.Error{Op: method, URL: target, Err: ErrEnqueueRedirect}
	}

	body := ""
	if !headRequest {
		f.mu.Lock()
		body = f.pages[ctx.url.Path]
		f.mu.Unlock()
	}
	req := &http.Request{Method: method, URL: ctx.url}
	_ = req

	f.mu.Lock()
	label := ctx.url.Path
	f.mu.Unlock()

	return &http.Response{
		StatusCode: 200,
		Status:     "200 OK",
		Header:     make(http.Header),
		Body:       &trackedBody{Reader: strings.NewReader(body), f: f, label: label},
		Request:    req,
	}, nil
}

func (f *pauseFixture) RequestGet(ctx *URLContext, headRes *http.Response) bool {
	f.record("RequestGet:" + ctx.url.Path)
	return headRes.StatusCode >= 200 && headRes.StatusCode < 300
}

func (f *pauseFixture) RequestRobots(ctx *URLContext, robotAgent string) ([]byte, bool) {
	f.record("RequestRobots:" + ctx.url.Host)
	if f.requestRobotsCache {
		f.mu.Lock()
		body := f.robots[ctx.url.Host]
		f.mu.Unlock()
		return []byte(body), false
	}
	return nil, true
}

func (f *pauseFixture) FetchedRobots(ctx *URLContext, res *http.Response) {
	f.record("FetchedRobots:" + ctx.url.Host)
}

func (f *pauseFixture) Filter(ctx *URLContext, isVisited bool) bool {
	f.record("Filter:" + ctx.url.Path)
	return !isVisited
}

func (f *pauseFixture) Enqueued(ctx *URLContext) {
	f.record("Enqueued:" + ctx.url.Path)
}

func (f *pauseFixture) Visit(ctx *URLContext, res *http.Response, doc *goquery.Document) (interface{}, bool) {
	f.mu.Lock()
	f.appendCall("Visit:" + ctx.url.Path)
	f.visits[ctx.url.Path]++
	f.visitState[ctx.url.Path] = ctx.State
	if ctx.SourceURL() != nil {
		f.visitSource[ctx.url.Path] = ctx.SourceURL().Path
	} else {
		f.visitSource[ctx.url.Path] = ""
	}
	f.mu.Unlock()
	return nil, true
}

func (f *pauseFixture) Visited(ctx *URLContext, harvested interface{}) {
	f.record("Visited:" + ctx.url.Path)
}

func (f *pauseFixture) Disallowed(ctx *URLContext) {
	f.record("Disallowed:" + ctx.url.Path)
}

// workerHookCalls counts every hook a worker may call during a phase.
func (f *pauseFixture) workerHookCalls() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	n := 0
	for _, c := range f.calls {
		switch {
		case strings.HasPrefix(c, "ComputeDelay"),
			strings.HasPrefix(c, "RequestRobots"),
			strings.HasPrefix(c, "Fetch"),
			strings.HasPrefix(c, "RequestGet"),
			strings.HasPrefix(c, "FetchedRobots"),
			strings.HasPrefix(c, "Visit"),
			strings.HasPrefix(c, "Visited"),
			strings.HasPrefix(c, "Disallowed"),
			strings.HasPrefix(c, "body-close"):
			n++
		}
	}
	return n
}

// callIndex returns the index of the first call matching s (exact or s:),
// or -1.
func callIndex(calls []string, s string) int {
	for i, c := range calls {
		if c == s || strings.HasPrefix(c, s+":") {
			return i
		}
	}
	return -1
}

// pauseCrawler builds a crawler over the fixture with the given option tweak.
func pauseCrawler(f *pauseFixture, tweak func(*Options)) *Crawler {
	o := NewOptions(f)
	o.LogFlags = 0
	o.UserAgent = "pause-test"
	o.RobotUserAgent = "pause-test"
	o.CrawlDelay = 0
	o.WorkerIdleTTL = 0
	if tweak != nil {
		tweak(o)
	}
	return NewCrawlerWithOptions(o)
}

// runAsync starts a Run in a goroutine and returns its error channel.
func runAsync(c *Crawler, seeds interface{}) <-chan error {
	ch := make(chan error, 1)
	go func() { ch <- c.Run(seeds) }()
	return ch
}

// waitErr waits for a Run error with a generous timeout.
func waitErr(t *testing.T, ch <-chan error, what string) error {
	t.Helper()
	select {
	case err := <-ch:
		return err
	case <-time.After(10 * time.Second):
		t.Fatalf("timed out waiting for %s", what)
		return nil
	}
}

// waitCond polls a predicate. It is only used by the tests to synchronize
// with observable state; the crawler itself never polls.
func waitCond(limit time.Duration, cond func() bool) bool {
	deadline := time.Now().Add(limit)
	for time.Now().Before(deadline) {
		if cond() {
			return true
		}
		time.Sleep(2 * time.Millisecond)
	}
	return false
}

// pauseAndWait starts a Pause, waits until admission is closed, and returns
// the pause result channel (it is not yet received).
func pauseAndWait(t *testing.T, c *Crawler) <-chan error {
	t.Helper()
	paused := make(chan error, 1)
	go func() { paused <- c.Pause() }()
	if !waitCond(5*time.Second, func() bool { return !c.gate.isOpen() }) {
		t.Fatalf("Pause never closed admission")
	}
	return paused
}

func receivePause(t *testing.T, ch <-chan error) error {
	t.Helper()
	select {
	case err := <-ch:
		return err
	case <-time.After(10 * time.Second):
		t.Fatalf("Pause did not return")
		return nil
	}
}

func gateActive(c *Crawler) (active, parked int) {
	c.gate.mu.Lock()
	defer c.gate.mu.Unlock()
	return c.gate.active, c.gate.parked
}

func workerCount(c *Crawler) int {
	active, _ := gateActive(c)
	return active
}

// -----------------------------------------------------------------------------

// TestCrawlPauseBarrier verifies the phase barrier: a pause succeeds while a
// user Fetch is still running, returns only once the phase has fully handed
// over its result, and no worker hook runs between Pause and Resume. It also
// checks that a HEAD and its GET are separate phases, that a HEAD is not
// repeated after a resume, and that a robots request is a phase of its own.
func TestCrawlPauseBarrier(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// The robots request itself is a phase: a pause inside it completes
		// normally and is observed as quiescent.
		f := newPauseFixture()
		f.pages["/page"] = `<p>page</p>`
		crawler := pauseCrawler(f, func(o *Options) {
			o.CrawlDelay = 0
		})
		f.robots["a.test"] = "User-agent: *\nAllow: /\n"
		runErr := runAsync(crawler, "http://a.test/page")
		f.blockOn("GET /robots.txt")
		f.waitEntered(t, "GET /robots.txt")
		paused := pauseAndWait(t, crawler)
		c.Check(f.bodiesClosed == 0, "robots body closed before its Fetch finished")
		f.release("GET /robots.txt")
		if err := receivePause(t, paused); err != nil {
			c.Check(false, "Pause during the robots phase failed: %v", err)
			return
		}
		c.Check(true, "robots phase is a pauseable phase")
		c.Check(f.bodiesClosed == 1, "the robots body must be closed before Pause returns, got %d", f.bodiesClosed)
		c.Check(f.fetchCount("GET /robots.txt") == 1, "robots fetched %d times", f.fetchCount("GET /robots.txt"))
		c.Check(f.fetchCount("GET /page") == 0, "the page started while paused")
		c.Check(f.visitCount("/page") == 0, "the page was visited while paused")

		// The Run stays alive while paused: no End call and no natural
		// completion.
		beforeEnds := f.endsCount()
		time.Sleep(60 * time.Millisecond)
		c.Check(f.endsCount() == beforeEnds, "End was called while paused")
		c.Check(crawler.Pause() == nil, "a second Pause while paused must succeed")
		c.Check(crawler.Resume() == nil, "Resume failed")
		c.Check(waitErr(t, runErr, "robots run") == nil, "robots Run must end cleanly")
		c.Check(f.visitCount("/page") == 1, "the page must be visited after resume, got %d", f.visitCount("/page"))

		// A HEAD and its GET are distinct phases.
		f2 := newPauseFixture()
		f2.pages["/head"] = `<a href="/child">child</a>`
		f2.pages["/child"] = `<p>child</p>`
		crawler2 := pauseCrawler(f2, func(o *Options) {
			o.HeadBeforeGet = true
			o.SameHostOnly = true
			o.CrawlDelay = 0
			o.WorkerIdleTTL = 60 * time.Millisecond
		})
		f2.requestRobotsCache = true
		f2.robots["a.test"] = "User-agent: *\nAllow: /\n"

		runErr2 := runAsync(crawler2, "http://a.test/head")
		f2.blockOn("HEAD /head")
		f2.waitEntered(t, "HEAD /head")
		paused2 := pauseAndWait(t, crawler2)
		select {
		case err := <-paused2:
			c.Check(false, "Pause returned while a Fetch was still blocked (err=%v)", err)
		default:
			c.Check(true, "Pause waits for an in-flight phase")
		}
		c.Check(f2.bodiesClosed == 0, "HEAD body closed while its Fetch was blocked")

		f2.release("HEAD /head")
		if err := receivePause(t, paused2); err != nil {
			c.Check(false, "Pause failed: %v", err)
			return
		}
		c.Check(true, "Pause succeeded after the in-flight phase completed")
		c.Check(f2.bodiesClosed == 1, "expected 1 closed body (the HEAD), got %d", f2.bodiesClosed)
		c.Check(f2.countCall("RequestGet:/head") == 1, "RequestGet must run inside the HEAD phase")
		c.Check(f2.fetchCount("GET /head") == 0, "GET started while paused: %d", f2.fetchCount("GET /head"))

		before := f2.workerHookCalls()
		time.Sleep(100 * time.Millisecond)
		c.Check(f2.workerHookCalls() == before, "worker hooks ran while paused: %d -> %d", before, f2.workerHookCalls())
		c.Check(f2.endsCount() == 0, "End was called while paused")

		c.Check(crawler2.Pause() == nil, "repeated Pause must succeed")
		if err := crawler2.Resume(); err != nil {
			c.Check(false, "Resume failed: %v", err)
		}
		c.Check(true, "Resume succeeded")
		c.Check(crawler2.Resume() == nil, "repeated Resume must succeed")

		err := waitErr(t, runErr2, "run completion")
		c.Check(err == nil, "expected a clean Run, got %v", err)
		c.Check(f2.startsCount() == 1 && f2.endsCount() == 1, "Start/End must run exactly once, got %d/%d",
			f2.startsCount(), f2.endsCount())
		c.Check(f2.fetchCount("HEAD /head") == 1, "HEAD issued %d times", f2.fetchCount("HEAD /head"))
		c.Check(f2.fetchCount("GET /head") == 1, "GET issued %d times", f2.fetchCount("GET /head"))
		c.Check(f2.visitCount("/head") == 1, "page visited %d times", f2.visitCount("/head"))
		c.Check(f2.visitCount("/child") == 1, "harvested link visited %d times after resume", f2.visitCount("/child"))
	})
}

// TestCrawlPauseContinuation verifies that a pause preserves the whole job:
// the tail of a batch already handed to a worker, URLs still in the pop
// channel, links harvested by a phase that was in flight, a redirect sent on
// EnqueueChan and a new site accepted during the pause. After a resume every
// accepted URL is processed exactly once, in the original per-site order,
// with the same URLContext state and source identity, without being re-filtered
// or re-notified.
func TestCrawlPauseContinuation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := newPauseFixture()
		f.pages["/two"] = `<a href="/three">three</a><a href="/four">four</a><a href="/five">five</a>`
		f.pages["/three"] = `<p>three</p>`
		f.pages["/four"] = `<p>four</p>`
		f.pages["/five"] = `<p>five</p>`
		f.pages["/redir"] = `<p>redir</p>`
		f.pages["/new"] = `<p>new</p>`
		f.redirects["/moved"] = "/redir"

		crawler := pauseCrawler(f, func(o *Options) {
			o.SameHostOnly = false
			o.CrawlDelay = 0
			o.WorkerIdleTTL = 0
		})
		f.requestRobotsCache = true
		f.robots["a.test"] = "User-agent: *\nAllow: /\n"
		f.robots["b.test"] = "User-agent: *\nAllow: /\n"

		// Two seeds of the same site: the worker gets them as one batch, so
		// /moved is the pending tail of that batch when the pause begins.
		runErr := runAsync(crawler, []string{"http://a.test/two", "http://a.test/moved"})
		f.blockOn("GET /two")
		f.waitEntered(t, "GET /two")

		// The first site's GET is in flight; the next batch item is pending.
		paused := pauseAndWait(t, crawler)

		// While quiescence is being established, the master still accepts
		// EnqueueChan input: a new site with a state, and a duplicate of an
		// already visited URL.
		select {
		case f.EnqueueChan <- map[string]interface{}{"http://b.test/new": "b-state"}:
		default:
			c.Check(false, "EnqueueChan was already wired")
		}
		select {
		case f.EnqueueChan <- "http://a.test/two":
		default:
			c.Check(false, "EnqueueChan was already wired")
		}
		c.Check(waitCond(3*time.Second, func() bool { return f.countCall("Enqueued:/new") == 1 }),
			"the URL accepted during the pause must be Enqueued")

		// Release the in-flight GET. Its Visit harvests /three,/four,/five,
		// which the master accepts before declaring the crawler quiescent.
		f.release("GET /two")
		if err := receivePause(t, paused); err != nil {
			c.Check(false, "Pause failed: %v", err)
			return
		}
		c.Check(true, "Pause succeeded with batch tail and pop queue preserved")
		c.Check(waitCond(2*time.Second, func() bool { return f.countCall("Enqueued:/five") == 1 }),
			"links of the phase that was in flight must be preserved")
		c.Check(f.visitCount("/two") == 1, "in-flight URL must be fully visited before Pause returns, got %d", f.visitCount("/two"))
		c.Check(f.fetchCount("GET /three") == 0 && f.fetchCount("GET /moved") == 0,
			"queued URLs must not start while paused")

		// A new site's worker may have been launched, but no robot phase may
		// start while paused.
		c.Check(f.countCall("RequestRobots:b.test") == 0, "robots request for the new site started while paused")
		c.Check(f.fetchCount("GET /new") == 0, "new-site page fetched while paused")

		// No worker-side hook runs while paused.
		beforeHooks := f.workerHookCalls()
		time.Sleep(80 * time.Millisecond)
		c.Check(f.workerHookCalls() == beforeHooks, "worker hooks ran while paused: %d -> %d",
			beforeHooks, f.workerHookCalls())

		if err := crawler.Resume(); err != nil {
			c.Check(false, "Resume failed: %v", err)
		}
		c.Check(true, "Resume succeeded")

		err := waitErr(t, runErr, "run completion")
		calls := f.snapshotCalls()
		t.Logf("trace: %v", calls)
		c.Check(err == nil, "expected a clean Run, got %v", err)
		c.Check(f.startsCount() == 1 && f.endsCount() == 1, "Start/End must run once, got %d/%d",
			f.startsCount(), f.endsCount())

		// Every URL is fetched and visited exactly once; the duplicate is
		// neither refetched nor renotified.
		for _, p := range []string{"/two", "/three", "/four", "/five", "/moved", "/redir", "/new"} {
			c.Check(f.fetchCount("GET "+p) == 1, "%s fetched %d times", p, f.fetchCount("GET "+p))
		}
		for _, p := range []string{"/two", "/three", "/four", "/five", "/redir", "/new"} {
			c.Check(f.visitCount(p) == 1, "%s visited %d times", p, f.visitCount(p))
		}
		c.Check(f.fetchCount("GET /two") == 1, "duplicate URL fetched again: %d", f.fetchCount("GET /two"))

		// Accepted URLs were filtered once each: a resume does not re-filter.
		for _, p := range []string{"/three", "/four", "/five", "/redir", "/new"} {
			c.Check(f.countCall("Filter:"+p) == 1, "%s filtered %d times", p, f.countCall("Filter:"+p))
		}

		// Site order: the harvested links keep their document order, and the
		// redirect is processed after its source.
		i3, i4, i5 := callIndex(calls, "Fetch:GET /three"), callIndex(calls, "Fetch:GET /four"), callIndex(calls, "Fetch:GET /five")
		im, ir := callIndex(calls, "Fetch:GET /moved"), callIndex(calls, "Fetch:GET /redir")
		c.Check(i3 >= 0 && i3 < i4 && i4 < i5, "harvested links lost their order: %d,%d,%d", i3, i4, i5)
		c.Check(im >= 0 && ir > im, "redirect processed before its source: %d,%d", im, ir)

		// URLContext identity: state and source are preserved across the pause.
		f.mu.Lock()
		newState, newSource := f.visitState["/new"], f.visitSource["/new"]
		threeSource := f.visitSource["/three"]
		redirSource := f.visitSource["/redir"]
		f.mu.Unlock()
		c.Check(newState == "b-state", "new-site state lost: %v", newState)
		c.Check(newSource == "", "new-site source identity changed: %q", newSource)
		c.Check(threeSource == "/two", "harvested link source lost: %q", threeSource)
		c.Check(redirSource == "/moved", "redirect source identity lost: %q", redirSource)

		// Repeated pause/resume cycles keep the same job: robots are resolved
		// once per site, the ComputeDelay last-request information is
		// continuous, and no visited URL is fetched or visited again.
		c.Check(f.fetchCount("GET /robots.txt") == 0, "robots came from cache but were fetched %d times", f.fetchCount("GET /robots.txt"))
		c.Check(f.countCall("RequestRobots:a.test") == 1, "robots requested %d times for a.test", f.countCall("RequestRobots:a.test"))
		c.Check(f.countCall("RequestRobots:b.test") == 1, "robots requested %d times for b.test", f.countCall("RequestRobots:b.test"))

		// The last FetchInfo seen by ComputeDelay after the resume is a real
		// preceding request, not a reset to nil.
		f.mu.Lock()
		var lastCompute string
		for _, call := range f.calls {
			if strings.HasPrefix(call, "ComputeDelay:") {
				lastCompute = call
			}
		}
		f.mu.Unlock()
		c.Check(lastCompute != "ComputeDelay:nil", "ComputeDelay lost the previous request information across the pause: %q", lastCompute)
		c.Check(f.visitCount("/two") == 1, "the seed was visited %d times", f.visitCount("/two"))
	})
}

// TestCrawlPauseLifecycle covers the control contract: ErrNotRunning outside
// a Run, concurrent Pause calls, Resume semantics, Stop races while pausing
// and while paused, a new Run not inheriting control signals, and the
// freezing of CrawlDelay and WorkerIdleTTL across pauses.
func TestCrawlPauseLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Not running: both control calls fail with ErrNotRunning.
		{
			f := newPauseFixture()
			crawler := pauseCrawler(f, nil)
			c.Check(crawler.Pause() == ErrNotRunning, "Pause before Run: %v", crawler.Pause())
			c.Check(crawler.Resume() == ErrNotRunning, "Resume before Run: %v", crawler.Resume())
			c.Check(crawler.Pause() == ErrNotRunning, "Pause outside a Run must report ErrNotRunning")
			c.Check(crawler.Resume() == ErrNotRunning, "Resume outside a Run must report ErrNotRunning")
		}

		// Two Pause calls wait for the same quiescence round.
		{
			f := newPauseFixture()
			f.pages["/p"] = `<p>p</p>`
			crawler := pauseCrawler(f, func(o *Options) { o.WorkerIdleTTL = 0 })
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, "http://a.test/p")
			f.blockOn("GET /p")
			f.waitEntered(t, "GET /p")

			p1 := pauseAndWait(t, crawler)
			p2 := make(chan error, 1)
			go func() { p2 <- crawler.Pause() }()
			f.release("GET /p")
			c.Check(receivePause(t, p1) == nil, "first Pause failed")
			c.Check(receivePause(t, p2) == nil, "second concurrent Pause failed")
			c.Check(crawler.Resume() == nil, "Resume after concurrent Pauses failed")
			c.Check(waitErr(t, runErr, "run completion") == nil, "run after resume")
		}

		// Stop interrupts a waiting Pause and ends the Run.
		{
			f := newPauseFixture()
			f.pages["/s"] = `<p>s</p>`
			crawler := pauseCrawler(f, nil)
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, "http://a.test/s")
			f.blockOn("GET /s")
			f.waitEntered(t, "GET /s")
			paused := pauseAndWait(t, crawler)

			crawler.Stop()
			err := receivePause(t, paused)
			c.Check(err == ErrInterrupted, "a stopped Pause must return ErrInterrupted, got %v", err)
			f.release("GET /s")
			c.Check(waitErr(t, runErr, "run after stop") == ErrInterrupted, "Run must report ErrInterrupted")

			// A second Stop is harmless and a post-run Pause is NotRunning.
			crawler.Stop()
			c.Check(true, "second Stop must not panic")
			c.Check(crawler.Pause() == ErrNotRunning, "Pause after Run must be ErrNotRunning")
			c.Check(crawler.Resume() == ErrNotRunning, "Resume after Run must be ErrNotRunning")
		}

		// Stop while paused ends the Run and releases control.
		{
			f := newPauseFixture()
			f.pages["/a"] = `<p>a</p>`
			f.pages["/b"] = `<p>b</p>`
			crawler := pauseCrawler(f, nil)
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, []string{"http://a.test/a", "http://a.test/b"})
			f.blockOn("GET /a")
			f.waitEntered(t, "GET /a")
			paused := pauseAndWait(t, crawler)
			f.release("GET /a")
			c.Check(receivePause(t, paused) == nil, "Pause failed")

			start := time.Now()
			crawler.Stop()
			err := waitErr(t, runErr, "run after stop while paused")
			elapsed := time.Since(start)
			c.Check(err == ErrInterrupted, "Run after Stop must be ErrInterrupted, got %v", err)
			c.Check(elapsed < 2*time.Second, "Stop while paused took %v", elapsed)
			c.Check(crawler.Pause() == ErrNotRunning, "Pause after a stopped Run must be ErrNotRunning")
			c.Check(crawler.Resume() == ErrNotRunning, "Resume after a stopped Run must be ErrNotRunning")
		}

		// A long CrawlDelay must not delay Pause or Stop.
		{
			f := newPauseFixture()
			f.pages["/x"] = `<p>x</p>`
			f.pages["/y"] = `<p>y</p>`
			crawler := pauseCrawler(f, func(o *Options) { o.CrawlDelay = 2 * time.Hour })
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, []string{"http://a.test/x", "http://a.test/y"})
			f.blockOn("GET /x")
			f.waitEntered(t, "GET /x")
			f.release("GET /x")
			c.Check(waitCond(2*time.Second, func() bool { return f.fetchCount("GET /x") == 1 }),
				"first fetch must have happened")

			start := time.Now()
			pauseErr := crawler.Pause()
			pauseElapsed := time.Since(start)
			c.Check(pauseErr == nil, "Pause with a huge crawl delay failed: %v", pauseErr)
			c.Check(pauseElapsed < 2*time.Second, "Pause waited for the crawl delay: %v", pauseElapsed)
			c.Check(f.fetchCount("GET /y") == 0, "the delayed URL was fetched while paused")

			start = time.Now()
			crawler.Stop()
			err := waitErr(t, runErr, "run after stop with long delay")
			stopElapsed := time.Since(start)
			c.Check(err == ErrInterrupted, "Run must report ErrInterrupted, got %v", err)
			c.Check(stopElapsed < 2*time.Second, "Stop waited for the crawl delay: %v", stopElapsed)
		}

		// CrawlDelay remainder is frozen across the pause: the next request
		// waits out the remainder instead of firing immediately or restarting.
		{
			f := newPauseFixture()
			f.pages["/d1"] = `<p>d1</p>`
			f.pages["/d2"] = `<p>d2</p>`
			const delay = 500 * time.Millisecond
			crawler := pauseCrawler(f, func(o *Options) { o.CrawlDelay = delay })
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, []string{"http://a.test/d1", "http://a.test/d2"})
			c.Check(waitCond(3*time.Second, func() bool { return f.fetchCount("GET /d1") == 1 }),
				"first fetch must happen")
			time.Sleep(60 * time.Millisecond) // leave about 440ms of delay

			start := time.Now()
			pauseErr := crawler.Pause()
			pauseElapsed := time.Since(start)
			c.Check(pauseErr == nil, "Pause during the crawl delay failed: %v", pauseErr)
			c.Check(pauseElapsed < 250*time.Millisecond,
				"Pause waited for the remaining crawl delay instead of parking: %v", pauseElapsed)

			// Pausing longer than the whole delay must not license an
			// immediate request-based resume.
			time.Sleep(delay + 200*time.Millisecond)
			resumedAt := time.Now()
			c.Check(crawler.Resume() == nil, "Resume failed")
			c.Check(waitCond(3*time.Second, func() bool { return f.fetchCount("GET /d2") == 1 }),
				"the delayed URL must be fetched after resume")
			times := f.lastFetchTimes("GET /d1")
			times2 := f.lastFetchTimes("GET /d2")
			d1 := times[len(times)-1]
			d2 := times2[len(times2)-1]
			c.Check(d2.Sub(resumedAt) >= 150*time.Millisecond,
				"resume fired the delayed request immediately (%v after resume)", d2.Sub(resumedAt))
			c.Check(d2.Sub(d1) >= delay-100*time.Millisecond,
				"crawl delay not honored across the pause: %v", d2.Sub(d1))
			c.Check(waitErr(t, runErr, "run completion") == nil, "run must end cleanly")
			_ = f
		}

		// WorkerIdleTTL is frozen across the pause: a worker idle when paused
		// survives a pause longer than its TTL.
		{
			f := newPauseFixture()
			f.pages["/busy"] = `<a href="/never">never</a>`
			f.pages["/other"] = `<p>other</p>`
			f.pages["/late"] = `<p>late</p>`
			const ttl = 300 * time.Millisecond
			crawler := pauseCrawler(f, func(o *Options) { o.WorkerIdleTTL = ttl })
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			f.robots["b.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, []string{"http://a.test/busy", "http://b.test/other"})
			f.blockOn("GET /busy")
			f.waitEntered(t, "GET /busy")
			paused := pauseAndWait(t, crawler)
			f.release("GET /busy")
			c.Check(receivePause(t, paused) == nil, "Pause failed")
			c.Check(workerCount(crawler) == 2, "expected both workers before the pause, got %d", workerCount(crawler))

			// Keep the job alive past the resume with a URL accepted during
			// the pause.
			select {
			case f.EnqueueChan <- "http://b.test/late":
			default:
				c.Check(false, "EnqueueChan missing")
			}
			c.Check(waitCond(2*time.Second, func() bool { return f.countCall("Enqueued:/late") == 1 }),
				"the late URL was not accepted while paused")

			// The pause outlives the idle TTL of the idle worker; it must not
			// be destroyed, and the other worker's crawl-delay state must
			// survive too.
			time.Sleep(ttl + 250*time.Millisecond)
			c.Check(workerCount(crawler) == 2, "idle TTL destroyed a worker while paused: %d", workerCount(crawler))

			c.Check(crawler.Resume() == nil, "Resume failed")

			// The preserved idle worker is still usable after the resume: the
			// URL accepted during the pause is processed.
			c.Check(waitCond(3*time.Second, func() bool { return f.visitCount("/late") == 1 }),
				"the preserved worker did not process the late URL")

			// Its idle TTL resumes from the frozen remainder: the worker must
			// not expire immediately on resume, nor wait for a brand-new TTL
			// (the pause itself already outlived the whole TTL).
			deadline := time.Now().Add(2 * time.Second)
			for time.Now().Before(deadline) && workerCount(crawler) == 2 {
				time.Sleep(2 * time.Millisecond)
			}
			c.Check(workerCount(crawler) < 2, "idle workers never expired after resume")

			c.Check(waitErr(t, runErr, "run completion") == nil, "run must end cleanly")
		}

		// A new Run on the same Crawler does not inherit control signals.
		{
			f := newPauseFixture()
			f.pages["/r"] = `<p>r</p>`
			crawler := pauseCrawler(f, nil)
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, "http://a.test/r")
			f.blockOn("GET /r")
			f.waitEntered(t, "GET /r")
			crawler.Stop()
			f.release("GET /r")
			c.Check(waitErr(t, runErr, "first run") == ErrInterrupted, "first Run must be interrupted")

			// A fresh Run starts from a clean control state and can pause.
			runErr = runAsync(crawler, "http://a.test/r")
			f.blockOn("GET /r")
			f.waitEntered(t, "GET /r")
			paused := pauseAndWait(t, crawler)
			f.release("GET /r")
			c.Check(receivePause(t, paused) == nil, "Pause in a second Run failed")
			c.Check(crawler.Resume() == nil, "Resume in a second Run failed")
			c.Check(waitErr(t, runErr, "second run") == nil, "second Run must end cleanly")
			c.Check(f.startsCount() == 2 && f.endsCount() == 2, "each Run must call Start/End once, got %d/%d",
				f.startsCount(), f.endsCount())
			c.Check(f.fetchCount("GET /r") == 2, "a second Run must visit again (dedup is per Run), got %d",
				f.fetchCount("GET /r"))
		}

		// A crawl that ends naturally at the same time as a pause either
		// reports success while keeping the Run alive, or ErrNotRunning; it
		// never reports a pause success after Run has returned.
		{
			f := newPauseFixture()
			f.pages["/done"] = `<p>done</p>`
			crawler := pauseCrawler(f, nil)
			f.requestRobotsCache = true
			f.robots["a.test"] = "User-agent: *\nAllow: /\n"
			runErr := runAsync(crawler, "http://a.test/done")
			f.blockOn("GET /done")
			f.waitEntered(t, "GET /done")

			paused := make(chan error, 1)
			go func() { paused <- crawler.Pause() }()
			f.release("GET /done")

			// At least one of the two outcomes must be consistent: if the
			// pause succeeded, the Run must still be alive at that instant.
			select {
			case pauseErr := <-paused:
				if pauseErr == nil {
					c.Check(!isRunFinished(runErr), "Pause reported success after Run had ended")
					c.Check(crawler.Resume() == nil, "Resume after a successful terminal Pause failed")
					c.Check(waitErr(t, runErr, "terminal run") == nil, "Run must end cleanly")
				} else {
					c.Check(pauseErr == ErrNotRunning || pauseErr == ErrInterrupted,
						"unexpected Pause error at natural end: %v", pauseErr)
					c.Check(waitErr(t, runErr, "terminal run") == nil, "Run must end cleanly")
				}
			case <-time.After(5 * time.Second):
				c.Check(false, "Pause and the natural end never resolved")
			}

			// After the Run is over, both control methods report NotRunning.
			c.Check(crawler.Pause() == ErrNotRunning, "Pause after Run must be ErrNotRunning")
			c.Check(crawler.Resume() == ErrNotRunning, "Resume after Run must be ErrNotRunning")
		}
	})
}

// isRunFinished reports whether the Run error channel has already produced
// its value.
func isRunFinished(ch <-chan error) bool {
	select {
	case <-ch:
		return true
	default:
		return false
	}
}
