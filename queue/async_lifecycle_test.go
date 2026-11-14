package queue

import (
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/cookiejar"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gocolly/colly/v2"
	"github.com/gocolly/colly/v2/task_support/taskcheck"
)

// localSource is a gated stand-in for a local HTTP source: it implements
// http.RoundTripper so no network port has to be bound, while channels
// control when each response body is released, exactly like the baseline
// reproduction.
type localSource struct {
	mu sync.Mutex
	// active/peak cover every request, including external Collector visits.
	active int
	peak   int
	// queueActive/queuePeak count only requests classified as queue work,
	// so the Threads limit can be asserted independently of external visits
	// sharing the same Collector.
	queueActive int
	queuePeak   int
	isQueue     func(*http.Request) bool
	order       []string
	gates       map[string]chan struct{}
	fails       map[string]error
	cookies     map[string]string
}

func newLocalSource() *localSource {
	return &localSource{
		gates:   map[string]chan struct{}{},
		fails:   map[string]error{},
		cookies: map[string]string{},
	}
}

func (s *localSource) gate(path string) chan struct{} {
	s.mu.Lock()
	defer s.mu.Unlock()
	ch := make(chan struct{})
	s.gates[path] = ch
	return ch
}

func (s *localSource) fail(path string, err error) {
	s.mu.Lock()
	s.fails[path] = err
	s.mu.Unlock()
}

func (s *localSource) snapshot() (int, []string) {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.peak, append([]string(nil), s.order...)
}

func (s *localSource) queuePeakValue() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.queuePeak
}

func (s *localSource) enteredCount() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.order)
}

func (s *localSource) cookieFor(path string) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.cookies[path]
}

func (s *localSource) RoundTrip(r *http.Request) (*http.Response, error) {
	path := r.URL.Path
	s.mu.Lock()
	s.active++
	if s.active > s.peak {
		s.peak = s.active
	}
	queued := s.isQueue == nil || s.isQueue(r)
	if queued {
		s.queueActive++
		if s.queueActive > s.queuePeak {
			s.queuePeak = s.queueActive
		}
	}
	s.order = append(s.order, path)
	gate := s.gates[path]
	err := s.fails[path]
	if cookie := r.Header.Get("Cookie"); cookie != "" {
		s.cookies[path] = cookie
	}
	s.mu.Unlock()

	if gate != nil {
		<-gate
	}
	s.mu.Lock()
	s.active--
	if queued {
		s.queueActive--
	}
	s.mu.Unlock()
	if err != nil {
		return nil, err
	}
	header := http.Header{}
	header.Set("Content-Type", "text/plain")
	if path == "/set-cookie" {
		header.Set("Set-Cookie", "session=queue-17; Path=/")
	}
	body := fmt.Sprintf("body:%s", path)
	return &http.Response{
		StatusCode: http.StatusOK,
		Status:     "200 OK",
		Header:     header,
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    r,
	}, nil
}

func newQueuedCollector(source *localSource, options ...colly.CollectorOption) *colly.Collector {
	options = append(options, colly.IgnoreRobotsTxt())
	c := colly.NewCollector(options...)
	jar, _ := cookiejar.New(nil)
	c.SetClient(&http.Client{Transport: source, Jar: jar})
	return c
}

func runQueue(q *Queue, c *colly.Collector) chan error {
	ch := make(chan error, 1)
	go func() { ch <- q.Run(c) }()
	return ch
}

// waitFor polls condition until it holds or the timeout elapses.
func waitFor(timeout time.Duration, condition func() bool) bool {
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if condition() {
			return true
		}
		time.Sleep(5 * time.Millisecond)
	}
	return condition()
}

func assertRunPending(c *taskcheck.C, ch chan error, label string) {
	select {
	case err := <-ch:
		c.Check(false, "%s: Run returned early with err=%v while work was still in flight", label, err)
	case <-time.After(150 * time.Millisecond):
		c.Check(true, "")
	}
}

func assertRunReturns(c *taskcheck.C, ch chan error, timeout time.Duration) {
	select {
	case err := <-ch:
		c.Check(err == nil, "Run returned error %v, want nil", err)
	case <-time.After(timeout):
		c.Check(false, "Run did not return within %v", timeout)
	}
}

func containsPath(order []string, path string) bool {
	for _, p := range order {
		if p == path {
			return true
		}
	}
	return false
}

// gatedStorage wraps a Storage and blocks one GetRequest call after a
// configurable number of free pops, to reproduce the "taken from storage
// but not dispatched yet" stage.
type gatedStorage struct {
	Storage
	mu       sync.Mutex
	getGate  chan struct{}
	freeGets int
	popCount int
}

// armBlock lets the next freeGets pops pass and blocks the one after that
// until the returned channel is closed.
func (g *gatedStorage) armBlock(freeGets int) chan struct{} {
	g.mu.Lock()
	defer g.mu.Unlock()
	g.freeGets = freeGets
	g.getGate = make(chan struct{})
	return g.getGate
}

func (g *gatedStorage) GetRequest() ([]byte, error) {
	g.mu.Lock()
	gate := g.getGate
	if g.freeGets > 0 {
		g.freeGets--
		gate = nil
	}
	g.mu.Unlock()
	if gate != nil {
		<-gate
		g.mu.Lock()
		g.getGate = nil
		g.mu.Unlock()
	}
	b, err := g.Storage.GetRequest()
	g.mu.Lock()
	g.popCount++
	g.mu.Unlock()
	return b, err
}

func (g *gatedStorage) pops() int {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.popCount
}

// sizeObservingStorage records whether QueueSize ever reported an empty
// storage while the run was active.
type sizeObservingStorage struct {
	Storage
	mu       sync.Mutex
	sawEmpty bool
}

func (s *sizeObservingStorage) QueueSize() (int, error) {
	n, err := s.Storage.QueueSize()
	if n == 0 {
		s.mu.Lock()
		s.sawEmpty = true
		s.mu.Unlock()
	}
	return n, err
}

func (s *sizeObservingStorage) observedEmpty() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.sawEmpty
}

func mustParseURL(raw string) *url.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}

func TestQueueOwnCompletion(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const base = "http://catalog.test"
		source := newLocalSource()
		gates := map[string]chan struct{}{}
		for _, p := range []string{"/a", "/b", "/c"} {
			gates[p] = source.gate(p)
		}
		// Only the seeded queue URLs count toward the Threads peak.
		source.isQueue = func(r *http.Request) bool { return r.URL.Path != "/ext" }

		coll := newQueuedCollector(source, colly.Async(true), colly.AllowURLRevisit())
		var callbacks atomic.Int32
		coll.OnResponse(func(r *colly.Response) {
			c.Check(string(r.Body) == "body:"+r.Request.URL.Path,
				"callback got body %q, want %q", r.Body, "body:"+r.Request.URL.Path)
			callbacks.Add(1)
		})

		q, err := New(1, nil)
		c.Check(err == nil, "New returned error %v", err)
		for _, p := range []string{"/a", "/b", "/c"} {
			c.Check(q.AddURL(base+p) == nil, "AddURL failed")
		}

		// An external Visit on the same asynchronous Collector must stay
		// independent: it neither consumes a queue slot nor keeps Run from
		// returning.
		extGate := source.gate("/ext")
		var extCallback atomic.Bool
		coll.OnResponse(func(r *colly.Response) {
			if r.Request.URL.Path == "/ext" {
				extCallback.Store(true)
			}
		})
		go func() { _ = coll.Visit(base + "/ext") }()

		runCh := runQueue(q, coll)

		c.Check(waitFor(2*time.Second, func() bool { return source.enteredCount() >= 1 }),
			"first queue request never reached the source")
		// Threads=1: while the first body is held, no other queue request
		// may be dispatched, even though the Collector is asynchronous.
		time.Sleep(150 * time.Millisecond)
		_, order := source.snapshot()
		queueOrder := make([]string, 0, len(order))
		for _, p := range order {
			if p != "/ext" {
				queueOrder = append(queueOrder, p)
			}
		}
		c.Check(len(queueOrder) == 1 && queueOrder[0] == "/a",
			"dispatched %v before first request finished, want only /a", queueOrder)
		c.Check(source.queuePeakValue() == 1,
			"Threads=1 allowed queue concurrency peak %d, want 1", source.queuePeakValue())
		assertRunPending(c, runCh, "async held response")

		// The external Visit proceeds concurrently without taking the
		// queue's single slot.
		c.Check(waitFor(2*time.Second, func() bool {
			_, seen := source.snapshot()
			return containsPath(seen, "/ext")
		}), "external Visit never reached the source")
		c.Check(source.queuePeakValue() == 1,
			"external Visit inflated queue concurrency peak to %d", source.queuePeakValue())

		close(gates["/a"])
		c.Check(waitFor(2*time.Second, func() bool {
			_, seen := source.snapshot()
			return containsPath(seen, "/b")
		}), "/b was not dispatched after /a finished")
		c.Check(source.queuePeakValue() == 1,
			"queue concurrency peak grew to %d, want 1", source.queuePeakValue())
		assertRunPending(c, runCh, "after first release")
		close(gates["/b"])
		c.Check(waitFor(2*time.Second, func() bool { return source.enteredCount() >= 3 }),
			"/c was not dispatched")
		close(gates["/c"])

		// Run returns only after every queue callback has exited; the
		// external callback is still blocked and is not waited on.
		assertRunReturns(c, runCh, 2*time.Second)
		c.Check(callbacks.Load() == 3, "callbacks=%d when Run returned, want 3", callbacks.Load())
		c.Check(!extCallback.Load(), "Run waited on the external Collector request")

		close(extGate)
		coll.Wait()
		c.Check(extCallback.Load(), "external callback did not run after c.Wait()")

		// Ctx, Depth, body and cookies survive the queue round trip.
		propSource := newLocalSource()
		propColl := newQueuedCollector(propSource, colly.Async(true), colly.AllowURLRevisit())
		var gotDepth int
		var gotCtx string
		propColl.OnResponse(func(r *colly.Response) {
			if r.Request.URL.Path == "/set-cookie" {
				gotDepth = r.Request.Depth
				gotCtx = r.Ctx.Get("trace")
			}
		})
		propQ, _ := New(1, nil)
		ctx := colly.NewContext()
		ctx.Put("trace", "ctx-17")
		c.Check(propQ.AddRequest(&colly.Request{
			URL:     mustParseURL(base + "/set-cookie"),
			Method:  http.MethodGet,
			Depth:   2,
			Ctx:     ctx,
			Headers: &http.Header{},
		}) == nil, "property AddRequest failed")
		c.Check(propQ.AddURL(base+"/needs-cookie") == nil, "cookie AddURL failed")
		propErr := propQ.Run(propColl)
		c.Check(propErr == nil, "property Run errored: %v", propErr)
		c.Check(gotDepth == 2, "Depth=%d after queue round trip, want 2", gotDepth)
		c.Check(gotCtx == "ctx-17", "Ctx value=%q, want ctx-17", gotCtx)
		c.Check(waitFor(time.Second, func() bool { return propSource.cookieFor("/needs-cookie") != "" }),
			"cookie jar was not shared through the queue")

		// Synchronous Collector control: same completion and slot rules.
		syncSource := newLocalSource()
		syncGate := syncSource.gate("/s1")
		syncColl := newQueuedCollector(syncSource, colly.AllowURLRevisit())
		syncQ, _ := New(1, nil)
		c.Check(syncQ.AddURL(base+"/s1") == nil, "sync AddURL s1 failed")
		c.Check(syncQ.AddURL(base+"/s2") == nil, "sync AddURL s2 failed")
		syncCh := runQueue(syncQ, syncColl)
		c.Check(waitFor(2*time.Second, func() bool { return syncSource.enteredCount() == 1 }),
			"sync queue did not start")
		assertRunPending(c, syncCh, "sync held response")
		close(syncGate)
		assertRunReturns(c, syncCh, 2*time.Second)
		c.Check(syncSource.queuePeakValue() == 1, "sync peak=%d, want 1", syncSource.queuePeakValue())

		// HTTP/network errors still go through OnError and finish the work.
		errSource := newLocalSource()
		netErr := errors.New("source unavailable")
		errSource.fail("/fail", netErr)
		errColl := newQueuedCollector(errSource, colly.Async(true), colly.AllowURLRevisit())
		var errorCount atomic.Int32
		errColl.OnError(func(_ *colly.Response, err error) {
			c.Check(err != nil, "OnError called with nil error")
			errorCount.Add(1)
		})
		errQ, _ := New(1, nil)
		c.Check(errQ.AddURL(base+"/fail") == nil, "fail AddURL failed")
		c.Check(errQ.AddURL(base+"/ok") == nil, "ok AddURL failed")
		assertRunReturns(c, runQueue(errQ, errColl), 2*time.Second)
		c.Check(errorCount.Load() == 1, "OnError count=%d, want 1", errorCount.Load())

		// Abort in OnRequest and in OnResponseHeaders must release the slot.
		abortSource := newLocalSource()
		abortColl := newQueuedCollector(abortSource, colly.Async(true), colly.AllowURLRevisit())
		var abortErrors atomic.Int32
		abortColl.OnRequest(func(r *colly.Request) {
			if r.URL.Path == "/abort-req" {
				r.Abort()
			}
		})
		abortColl.OnResponseHeaders(func(r *colly.Response) {
			r.Request.Abort()
		})
		abortColl.OnError(func(_ *colly.Response, err error) {
			c.Check(errors.Is(err, colly.ErrAbortedAfterHeaders),
				"headers abort error=%v, want ErrAbortedAfterHeaders", err)
			abortErrors.Add(1)
		})
		abortQ, _ := New(2, nil)
		c.Check(abortQ.AddURL(base+"/abort-req") == nil, "abort-req AddURL failed")
		c.Check(abortQ.AddURL(base+"/abort-hdr") == nil, "abort-hdr AddURL failed")
		assertRunReturns(c, runQueue(abortQ, abortColl), 2*time.Second)
		c.Check(abortErrors.Load() == 1, "headers-abort OnError count=%d, want 1", abortErrors.Load())
		_, abortOrder := abortSource.snapshot()
		c.Check(len(abortOrder) == 1 && abortOrder[0] == "/abort-hdr",
			"OnRequest abort still hit the source: %v", abortOrder)

		// Dedup rejection finishes the dispatched work without a source hit.
		rejectSource := newLocalSource()
		rejectColl := newQueuedCollector(rejectSource, colly.Async(true))
		rejectQ, _ := New(1, nil)
		c.Check(rejectQ.AddURL(base+"/once") == nil, "dedup AddURL 1 failed")
		c.Check(rejectQ.AddURL(base+"/once") == nil, "dedup AddURL 2 failed")
		assertRunReturns(c, runQueue(rejectQ, rejectColl), 2*time.Second)
		c.Check(rejectSource.enteredCount() == 1,
			"dedup rejected request reached the source %d times, want 1", rejectSource.enteredCount())

		// Domain rejection likewise completes through the real queue path.
		domainSource := newLocalSource()
		domainColl := newQueuedCollector(domainSource, colly.Async(true),
			colly.AllowedDomains("example.com"), colly.AllowURLRevisit())
		domainQ, _ := New(1, nil)
		c.Check(domainQ.AddURL(base+"/forbidden") == nil, "forbidden AddURL failed")
		c.Check(domainQ.AddURL("http://example.com/allowed") == nil, "allowed AddURL failed")
		assertRunReturns(c, runQueue(domainQ, domainColl), 2*time.Second)
		_, domainOrder := domainSource.snapshot()
		c.Check(len(domainOrder) == 1 && domainOrder[0] == "/allowed",
			"domain filter not respected: %v", domainOrder)

		// An empty queue finishes immediately and can be re-run afterwards.
		emptyQ, _ := New(2, nil)
		emptyColl := newQueuedCollector(newLocalSource(), colly.Async(true))
		assertRunReturns(c, runQueue(emptyQ, emptyColl), 2*time.Second)
		c.Check(emptyQ.AddURL(base+"/after-empty") == nil, "AddURL after empty Run failed")
		assertRunReturns(c, runQueue(emptyQ, emptyColl), 2*time.Second)

		// A second concurrent Run keeps panicking instead of corrupting
		// the run bookkeeping. Pin the first Run with a held response so
		// the duplicate call is guaranteed to observe an active run.
		dupSource := newLocalSource()
		dupGate := dupSource.gate("/dup")
		dupQ, _ := New(1, nil)
		dupColl := newQueuedCollector(dupSource, colly.Async(true), colly.AllowURLRevisit())
		c.Check(dupQ.AddURL(base+"/dup") == nil, "dup AddURL failed")
		dupCh := runQueue(dupQ, dupColl)
		c.Check(waitFor(2*time.Second, func() bool { return dupSource.enteredCount() == 1 }),
			"pinned duplicate-run request never started")
		func() {
			defer func() {
				c.Check(recover() != nil, "duplicate Queue.Run did not panic")
			}()
			_ = dupQ.Run(dupColl)
		}()
		close(dupGate)
		assertRunReturns(c, dupCh, 2*time.Second)

		// Storage failures are returned by Run (unlike crawl errors, which
		// stay on OnError) and do not leave the queue unusable.
		failing := &errorStorage{Storage: &InMemoryQueueStorage{MaxSize: 100}}
		failQ, _ := New(1, failing)
		c.Check(failQ.AddURL(base+"/boom") == nil, "failing-storage seed failed")
		failCh := make(chan error, 1)
		go func() { failCh <- failQ.Run(newQueuedCollector(newLocalSource(), colly.Async(true))) }()
		select {
		case err := <-failCh:
			c.Check(err != nil, "Run with failing storage returned nil error")
		case <-time.After(2 * time.Second):
			c.Check(false, "Run with failing storage never returned")
		}
		recoverQ, _ := New(1, &InMemoryQueueStorage{MaxSize: 100})
		recoverColl := newQueuedCollector(newLocalSource(), colly.Async(true), colly.AllowURLRevisit())
		c.Check(recoverQ.AddURL(base+"/healthy") == nil, "recovery seed failed")
		assertRunReturns(c, runQueue(recoverQ, recoverColl), 2*time.Second)

		// The callback stage itself keeps the slot and the run open: Run
		// must not return while an OnResponse callback is still blocked,
		// even after the response body has fully arrived.
		cbSource := newLocalSource()
		cbBodyGate := cbSource.gate("/cb")
		callbackGate := make(chan struct{})
		cbQ, _ := New(1, nil)
		cbColl := newQueuedCollector(cbSource, colly.Async(true), colly.AllowURLRevisit())
		var callbackEntered atomic.Bool
		cbColl.OnResponse(func(r *colly.Response) {
			callbackEntered.Store(true)
			<-callbackGate
		})
		c.Check(cbQ.AddURL(base+"/cb") == nil, "callback-gate seed failed")
		cbRunCh := runQueue(cbQ, cbColl)
		// Release the body so the callback starts and blocks itself.
		c.Check(waitFor(2*time.Second, func() bool { return cbSource.enteredCount() == 1 }),
			"callback-gate request never started")
		close(cbBodyGate)
		c.Check(waitFor(2*time.Second, callbackEntered.Load),
			"blocked callback never entered")
		assertRunPending(c, cbRunCh, "callback still executing")
		close(callbackGate)
		assertRunReturns(c, cbRunCh, 2*time.Second)
	})
}

// errorStorage always fails QueueSize.
type errorStorage struct {
	Storage
}

func (e *errorStorage) QueueSize() (int, error) {
	return 0, errStorageFailed
}

var errStorageFailed = errors.New("simulated storage failure")

func TestQueueCallbackDiscovery(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const base = "http://discover.test"
		source := newLocalSource()
		rootGate := source.gate("/root")
		d1Gate := source.gate("/d1")

		store := &sizeObservingStorage{Storage: &InMemoryQueueStorage{MaxSize: 1000}}
		q, err := New(2, store)
		c.Check(err == nil, "New returned error %v", err)
		coll := newQueuedCollector(source, colly.Async(true), colly.AllowURLRevisit())

		var visited atomic.Int32
		var enqueueReturned atomic.Bool
		coll.OnResponse(func(r *colly.Response) {
			visited.Add(1)
			switch r.Request.URL.Path {
			case "/root":
				// Both discovery APIs must be served by the running queue,
				// and enqueueing returns before the next response is
				// released: /d1's body gate stays closed the whole time.
				c.Check(q.AddURL(base+"/d1") == nil, "AddURL discovery failed")
				built := &colly.Request{
					URL:     mustParseURL(base + "/d2"),
					Method:  http.MethodGet,
					Depth:   r.Request.Depth + 1,
					Ctx:     r.Ctx,
					Headers: &http.Header{},
				}
				c.Check(q.AddRequest(built) == nil, "AddRequest discovery failed")
				enqueueReturned.Store(true)
			case "/d1":
				// A second-generation link discovered from a discovered
				// page must still be drained before Run returns.
				c.Check(q.AddURL(base+"/d3") == nil, "chained AddURL failed")
			}
		})

		c.Check(q.AddURL(base+"/root") == nil, "seed AddURL failed")
		runCh := runQueue(q, coll)

		c.Check(waitFor(2*time.Second, func() bool { return source.enteredCount() == 1 }),
			"root request never started")
		// Storage can legitimately look empty while the root callback is
		// still about to append links.
		c.Check(waitFor(2*time.Second, store.observedEmpty),
			"storage never observed empty during in-flight callback")
		close(rootGate)

		c.Check(waitFor(2*time.Second, enqueueReturned.Load),
			"discovery enqueue did not return while next response was held")
		// At this point /d1 is dispatched and blocked on its body gate.
		c.Check(waitFor(2*time.Second, func() bool {
			_, order := source.snapshot()
			return containsPath(order, "/d1")
		}), "discovered /d1 was not dispatched by the running queue")
		assertRunPending(c, runCh, "discovery in flight")
		close(d1Gate)

		assertRunReturns(c, runCh, 2*time.Second)
		c.Check(visited.Load() == 4, "visited=%d responses, want 4 (root,d1,d2,d3)", visited.Load())
		peak, order := source.snapshot()
		c.Check(peak <= 2, "concurrency peak %d exceeds Threads=2", peak)
		want := map[string]bool{"/root": true, "/d1": true, "/d2": true, "/d3": true}
		got := map[string]bool{}
		for _, p := range order {
			got[p] = true
		}
		c.Check(fmt.Sprint(got) == fmt.Sprint(want), "visited set=%v, want %v", got, want)
	})
}

func TestQueueStopResume(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const base = "http://stop.test"
		source := newLocalSource()
		aGate := source.gate("/a")

		storage := &gatedStorage{Storage: &InMemoryQueueStorage{MaxSize: 1000}}
		// One worker keeps every stop/retain interleaving deterministic.
		q, err := New(1, storage)
		c.Check(err == nil, "New returned error %v", err)
		coll := newQueuedCollector(source, colly.Async(true), colly.AllowURLRevisit())

		var aDone atomic.Bool
		coll.OnResponse(func(r *colly.Response) {
			if r.Request.URL.Path == "/a" {
				// Links discovered while stopping must still be stored.
				c.Check(q.AddURL(base+"/e") == nil, "AddURL during stop failed")
				aDone.Store(true)
			}
		})

		for _, p := range []string{"/a", "/b", "/c", "/d"} {
			c.Check(q.AddURL(base+p) == nil, "seed AddURL %s failed", p)
		}

		// First run: /a is dispatched, then the loop's next GetRequest
		// (for /b) blocks inside the gated storage. Stop lands while the
		// request is in flight and the popped /b has not been dispatched.
		firstPopGate := storage.armBlock(1)
		runCh := runQueue(q, coll)
		c.Check(waitFor(2*time.Second, func() bool { return source.enteredCount() == 1 }),
			"the first request should be in flight with Threads=1")
		c.Check(waitFor(2*time.Second, func() bool { return storage.pops() == 1 }),
			"loop did not reach the blocked GetRequest")

		started := time.Now()
		q.Stop()
		c.Check(time.Since(started) < 100*time.Millisecond,
			"Stop took %v, it must return immediately", time.Since(started))

		// The dispatched request keeps running; Run waits for it and must
		// not return while its body is held, nor while /b's pop is blocked.
		assertRunPending(c, runCh, "after Stop with held response")
		c.Check(!aDone.Load(), "/a callback finished before its body was released")
		close(aGate)
		assertRunPending(c, runCh, "after /a finished while GetRequest is blocked")
		c.Check(aDone.Load(), "/a callback did not run to completion after Stop")

		// Releasing the blocked GetRequest hands /b to a loop that has
		// already stopped: it must be retained, not dispatched or lost.
		close(firstPopGate)
		assertRunReturns(c, runCh, 2*time.Second)

		size, err := q.Size()
		c.Check(err == nil, "Size errored: %v", err)
		c.Check(size == 3, "storage after stop=%d, want 3 (c, d and discovered e)", size)
		_, firstOrder := source.snapshot()
		c.Check(len(firstOrder) == 1 && firstOrder[0] == "/a",
			"stopped queue dispatched %v after Stop, want only /a", firstOrder)

		// Second run on the same Queue instance: the retained /b is sent
		// first without touching storage, and the armed gate blocks the
		// next GetRequest (for /c) so Stop lands while that pop is in
		// flight but the request is not dispatched yet.
		bGate := source.gate("/b")
		secondPopGate := storage.armBlock(0)
		resumeCh := runQueue(q, coll)

		c.Check(waitFor(2*time.Second, func() bool {
			_, order := source.snapshot()
			return containsPath(order, "/b")
		}), "retained /b was not processed first on resume")

		q.Stop()
		assertRunPending(c, resumeCh, "after resume Stop while /b is held")
		close(bGate)
		assertRunPending(c, resumeCh, "after /b finished while GetRequest is blocked")
		// Releasing the blocked pop hands /c to an already stopped loop:
		// it must be retained for the next Run rather than dispatched.
		close(secondPopGate)
		assertRunReturns(c, resumeCh, 2*time.Second)

		size, err = q.Size()
		c.Check(err == nil, "resume Size errored: %v", err)
		c.Check(size == 2, "storage after retained pop=%d, want 2 (d, e)", size)

		// Third Run: retained /c goes first, then whatever remains in
		// storage (/d and the discovered /e). Fresh run state guarantees
		// that stop/wake events from the previous runs cannot interfere.
		finalCh := runQueue(q, coll)
		assertRunReturns(c, finalCh, 2*time.Second)

		peak, fullOrder := source.snapshot()
		c.Check(peak == 1, "Threads=1 concurrency peak=%d, want 1", peak)
		wantSet := map[string]bool{"/a": true, "/b": true, "/c": true, "/d": true, "/e": true}
		gotSet := map[string]bool{}
		for _, p := range fullOrder {
			gotSet[p] = true
		}
		c.Check(fmt.Sprint(gotSet) == fmt.Sprint(wantSet),
			"resumed address set=%v, want %v", gotSet, wantSet)
		index := func(p string) int {
			for i, seen := range fullOrder {
				if seen == p {
					return i
				}
			}
			return -1
		}
		c.Check(index("/a") < index("/b") && index("/b") < index("/c") &&
			index("/c") < index("/d") && index("/d") < index("/e"),
			"resume order=%v, want strict a,b,c,d,e with retained items first", fullOrder)

		// A stopped queue can run again normally with brand new work.
		reuseQ, _ := New(1, nil)
		reuseSource := newLocalSource()
		reuseColl := newQueuedCollector(reuseSource, colly.Async(true), colly.AllowURLRevisit())
		var reuseCallbacks atomic.Int32
		reuseColl.OnResponse(func(*colly.Response) { reuseCallbacks.Add(1) })
		c.Check(reuseQ.AddURL("http://reuse.test/again") == nil, "reuse AddURL failed")
		assertRunReturns(c, runQueue(reuseQ, reuseColl), 2*time.Second)
		c.Check(reuseCallbacks.Load() == 1,
			"fresh Run after Stop processed %d requests, want 1", reuseCallbacks.Load())

		// Stop must also be callable from inside a queue callback itself.
		cbSource := newLocalSource()
		cbGate := cbSource.gate("/first")
		cbQ, _ := New(1, &InMemoryQueueStorage{MaxSize: 100})
		cbColl := newQueuedCollector(cbSource, colly.Async(true), colly.AllowURLRevisit())
		var cbDone atomic.Bool
		cbColl.OnResponse(func(r *colly.Response) {
			if r.Request.URL.Path == "/first" {
				// A link discovered at stop time still has to be stored.
				c.Check(cbQ.AddURL(base+"/later") == nil, "callback AddURL failed")
				cbQ.Stop()
				cbDone.Store(true)
			}
		})
		c.Check(cbQ.AddURL(base+"/first") == nil, "callback-stop seed failed")
		c.Check(cbQ.AddURL(base+"/second") == nil, "callback-stop seed 2 failed")
		cbCh := runQueue(cbQ, cbColl)
		assertRunPending(c, cbCh, "callback Stop while response is held")
		close(cbGate)
		assertRunReturns(c, cbCh, 2*time.Second)
		c.Check(cbDone.Load(), "callback that called Stop did not finish")
		// /second may still be in storage or already retained on the Queue;
		// either way nothing was dispatched after Stop.
		_, cbOrder := cbSource.snapshot()
		c.Check(len(cbOrder) == 1 && cbOrder[0] == "/first",
			"callback Stop dispatched extra work: %v", cbOrder)
		// The next Run delivers both the queued /second and the link stored
		// at stop time, proving nothing was lost and stop-time work persists.
		assertRunReturns(c, runQueue(cbQ, cbColl), 2*time.Second)
		_, cbOrder = cbSource.snapshot()
		c.Check(containsPath(cbOrder, "/later") && containsPath(cbOrder, "/second"),
			"post-stop Run did not process stored work: %v", cbOrder)
	})
}
