package ant_test

import (
	"context"
	"errors"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/yields/ant"
	"github.com/yields/ant/task_support/taskcheck"
)

// scraperFunc adapts a function into an ant.Scraper.
type scraperFunc func(context.Context, *ant.Page) (ant.URLs, error)

func (f scraperFunc) Scrape(ctx context.Context, p *ant.Page) (ant.URLs, error) {
	return f(ctx, p)
}

// fakeClient is a channel/function driven ant.Client, so the
// shutdown behavior can be exercised without binding a port.
type fakeClient struct {
	do func(*http.Request) (*http.Response, error)
}

func (f fakeClient) Do(req *http.Request) (*http.Response, error) {
	return f.do(req)
}

func okResponse(req *http.Request) *http.Response {
	return &http.Response{
		StatusCode: http.StatusOK,
		Status:     "200 OK",
		Request:    req,
		Header:     make(http.Header),
		Body:       io.NopCloser(strings.NewReader("<html></html>")),
	}
}

// countingQueue wraps a real ant.MemoryQueue and records every
// queue interaction through the public Queue interface.
type countingQueue struct {
	ant.Queue
	enqueued   atomic.Int64
	takes      atomic.Int64
	dones      atomic.Int64
	closes     atomic.Int64
	prefetched chan struct{}
	once       sync.Once
}

func (q *countingQueue) Enqueue(ctx context.Context, urls ant.URLs) error {
	err := q.Queue.Enqueue(ctx, urls)
	if err == nil {
		q.enqueued.Add(int64(len(urls)))
	}
	return err
}

func (q *countingQueue) Dequeue(ctx context.Context) (*ant.URL, error) {
	u, err := q.Queue.Dequeue(ctx)
	if err == nil {
		if q.takes.Add(1) == 2 && q.prefetched != nil {
			q.once.Do(func() { close(q.prefetched) })
		}
	}
	return u, err
}

func (q *countingQueue) Done(ctx context.Context, u *ant.URL) error {
	err := q.Queue.Done(ctx, u)
	q.dones.Add(1)
	return err
}

func (q *countingQueue) Close(ctx context.Context) error {
	q.closes.Add(1)
	return q.Queue.Close(ctx)
}

func mustURL(raw string) *ant.URL {
	u, err := url.Parse(raw)
	if err != nil {
		panic(err)
	}
	return u
}

// waitSignal waits for ch and records a content check about it.
func waitSignal(c *taskcheck.C, ch <-chan struct{}, timeout time.Duration, msg string) bool {
	select {
	case <-ch:
		c.Check(true, "%s", msg)
		return true
	case <-time.After(timeout):
		c.Check(false, "%s (timed out after %s)", msg, timeout)
		return false
	}
}

// runEngine invokes Run on a goroutine and returns its result channel.
func runEngine(eng *ant.Engine, ctx context.Context, urls ...string) <-chan error {
	finished := make(chan error, 1)
	go func() { finished <- eng.Run(ctx, urls...) }()
	return finished
}

// TestShutdownCause covers a first non-context failure happening
// on one worker while a sibling worker is inside a scraper that is
// waiting on the canceled context and still cleaning resources up.
func TestShutdownCause(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var (
			entered    = make(chan struct{})
			proceed    = make(chan struct{})
			canceled   = make(chan struct{})
			release    = make(chan struct{})
			cleaned    = make(chan struct{})
			proceedOne sync.Once
			releaseOne sync.Once
		)
		proceedFn := func() { proceedOne.Do(func() { close(proceed) }) }
		releaseFn := func() { releaseOne.Do(func() { close(release) }) }
		defer proceedFn()
		defer releaseFn()

		boom := errors.New("scraper boom")
		queue := &countingQueue{Queue: ant.MemoryQueue(2)}
		eng, err := ant.NewEngine(ant.EngineConfig{
			Impolite:    true,
			Workers:     2,
			Concurrency: 2,
			Queue:       queue,
			Fetcher: &ant.Fetcher{Client: fakeClient{do: func(req *http.Request) (*http.Response, error) {
				return okResponse(req), nil
			}}},
			Scraper: scraperFunc(func(ctx context.Context, p *ant.Page) (ant.URLs, error) {
				switch p.URL.Path {
				case "/block":
					close(entered)
					<-ctx.Done()
					close(canceled)
					// Keep cleaning resources after cancellation
					// until the test releases the scraper.
					<-release
					close(cleaned)
					return nil, ctx.Err()
				case "/fail":
					<-proceed
					return nil, boom
				default:
					return nil, nil
				}
			}),
		})
		c.Check(err == nil, "new engine: %v", err)

		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		finished := runEngine(eng, ctx,
			"http://example.test/fail",
			"http://example.test/block",
		)

		if !waitSignal(c, entered, 2*time.Second, "sibling scraper entered") {
			return
		}
		proceedFn()
		if !waitSignal(c, canceled, 2*time.Second, "sibling scraper observed cancellation") {
			return
		}

		// Run must stay blocked while the in-flight scraper is
		// still cleaning up: ignoring Wait would return here.
		select {
		case err := <-finished:
			c.Check(false, "Run returned before scraper cleanup finished: %v", err)
		case <-time.After(100 * time.Millisecond):
			c.Check(true, "Run waits for the in-flight scraper cleanup")
		}

		releaseFn()
		if !waitSignal(c, cleaned, 2*time.Second, "scraper cleanup completed") {
			return
		}

		select {
		case err := <-finished:
			c.Check(err != nil, "Run propagates the first failure")
			c.Check(errors.Is(err, boom), "Run error wraps the first non-context failure: %v", err)
			c.Check(!errors.Is(err, context.Canceled),
				"the first failure is not masked by context.Canceled: %v", err)
		case <-time.After(2 * time.Second):
			c.Check(false, "Run returns after scraper cleanup (timed out)")
		}

		c.Check(queue.takes.Load() == 2, "both URLs were dequeued, got %d takes", queue.takes.Load())
		c.Check(queue.dones.Load() == queue.takes.Load(),
			"every dequeued URL was acknowledged exactly once: dones=%d takes=%d",
			queue.dones.Load(), queue.takes.Load())
	})
}

// TestShutdownPrefetch reproduces the original hang: one entry is
// inside the scraper while the next entry was already dequeued and
// is waiting for a concurrency slot when the context is canceled.
// It also keeps the normal-discovery control crawl.
func TestShutdownPrefetch(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		var (
			entered   = make(chan struct{})
			enterOnce sync.Once
		)
		queue := &countingQueue{
			Queue:      ant.MemoryQueue(2),
			prefetched: make(chan struct{}),
		}
		var requests atomic.Int64

		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()
		eng, err := ant.NewEngine(ant.EngineConfig{
			Impolite:    true,
			Workers:     1,
			Concurrency: 1,
			Queue:       queue,
			Fetcher: &ant.Fetcher{Client: fakeClient{do: func(req *http.Request) (*http.Response, error) {
				requests.Add(1)
				return okResponse(req), nil
			}}},
			Scraper: scraperFunc(func(ctx context.Context, p *ant.Page) (ant.URLs, error) {
				enterOnce.Do(func() { close(entered) })
				<-ctx.Done()
				return nil, ctx.Err()
			}),
		})
		c.Check(err == nil, "new engine: %v", err)

		finished := runEngine(eng, ctx,
			"http://example.test/one",
			"http://example.test/two",
		)

		if !waitSignal(c, entered, 2*time.Second, "first URL entered the scraper") {
			return
		}
		if !waitSignal(c, queue.prefetched, 2*time.Second, "second URL was prefetched from the queue") {
			return
		}

		cancel()
		select {
		case err := <-finished:
			c.Check(err != nil, "canceled Run returns an error")
			c.Check(errors.Is(err, context.Canceled),
				"external cancellation keeps the caller's context error: %v", err)
		case <-time.After(2 * time.Second):
			c.Check(false, "Run returned after cancellation (timed out)")
		}

		c.Check(queue.takes.Load() == 2, "both URLs were dequeued, got %d takes", queue.takes.Load())
		c.Check(queue.dones.Load() == 2,
			"the prefetched URL was acknowledged even though its request never started: dones=%d",
			queue.dones.Load())
		c.Check(requests.Load() == 1,
			"only the first URL started an HTTP request, got %d", requests.Load())

		// Control: a normal crawl still respects Concurrency and
		// keeps processing links discovered by pages.
		var visits atomic.Int32
		discEng, err := ant.NewEngine(ant.EngineConfig{
			Impolite:    true,
			Concurrency: 2,
			Fetcher: &ant.Fetcher{Client: fakeClient{do: func(req *http.Request) (*http.Response, error) {
				return okResponse(req), nil
			}}},
			Scraper: scraperFunc(func(_ context.Context, p *ant.Page) (ant.URLs, error) {
				visits.Add(1)
				if p.URL.Path == "/" {
					return ant.URLs{mustURL("http://example.test/child")}, nil
				}
				return nil, nil
			}),
		})
		c.Check(err == nil, "new discovery engine: %v", err)

		err = discEng.Run(context.Background(), "http://example.test/")
		c.Check(err == nil, "normal discovery crawl succeeds: %v", err)
		c.Check(visits.Load() == 2,
			"normal crawl visits the root and the discovered child, got %d", visits.Load())
	})
}

// TestQueueCancelWake covers MemoryQueue shutdown semantics:
// a blocked Dequeue wakes on context cancellation by itself, Close
// keeps returning io.EOF afterward, and repeated Close calls never
// account for the discarded entries twice.
func TestQueueCancelWake(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// A Dequeue parked on an empty queue must return the
		// context error without any Enqueue or Close wake-up.
		queue := ant.MemoryQueue(4)
		ctx, cancel := context.WithCancel(context.Background())

		type dequeueResult struct {
			u   *ant.URL
			err error
		}
		got := make(chan dequeueResult, 1)
		go func() {
			u, err := queue.Dequeue(ctx)
			got <- dequeueResult{u, err}
		}()

		// Let the goroutine park in cond.Wait before canceling.
		time.Sleep(50 * time.Millisecond)
		cancel()
		select {
		case r := <-got:
			c.Check(r.u == nil, "canceled Dequeue returns no URL")
			c.Check(errors.Is(r.err, context.Canceled),
				"canceled Dequeue returns context.Canceled on its own: %v", r.err)
		case <-time.After(2 * time.Second):
			c.Check(false, "Dequeue wakes when its context is canceled (timed out)")
		}

		// After Close, Dequeue keeps reporting io.EOF, even with
		// an already canceled context, and Close is idempotent.
		c.Check(queue.Close(context.Background()) == nil, "first Close succeeds")
		c.Check(queue.Close(context.Background()) == nil, "repeated Close is a no-op")

		canceledCtx, cancel2 := context.WithCancel(context.Background())
		cancel2()
		_, err := queue.Dequeue(canceledCtx)
		c.Check(errors.Is(err, io.EOF), "Dequeue after Close returns io.EOF, got %v", err)

		// Close must balance the WaitGroup for the discarded
		// pending entries, exactly once.
		balanced := ant.MemoryQueue(2)
		err = balanced.Enqueue(context.Background(), ant.URLs{
			mustURL("http://example.test/1"),
			mustURL("http://example.test/2"),
		})
		c.Check(err == nil, "enqueue two URLs: %v", err)

		dequeued, err := balanced.Dequeue(context.Background())
		c.Check(err == nil && dequeued.String() == "http://example.test/1",
			"first URL is dequeued, got %v / %v", dequeued, err)

		waitDone := make(chan struct{})
		go func() {
			balanced.Wait()
			close(waitDone)
		}()

		c.Check(balanced.Close(context.Background()) == nil,
			"Close discards the one pending URL")
		c.Check(balanced.Close(context.Background()) == nil,
			"second Close does not account for the discard again")
		c.Check(balanced.Done(context.Background(), dequeued) == nil,
			"the dequeued URL is acknowledged once")

		select {
		case <-waitDone:
			c.Check(true, "Wait returns once pending entries are discarded and taken entries are Done")
		case <-time.After(2 * time.Second):
			c.Check(false, "Wait blocked: Close did not balance the discarded URLs")
		}
	})
}
