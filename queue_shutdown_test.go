package fetchbot

import (
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"testing"
	"time"
)

// sdOK returns a stub Doer that answers every request with a 200 "ok" body.
func sdOK() stubDoer {
	return stubDoer(func(r *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(strings.NewReader("ok")), Request: r}, nil
	})
}

// waitForCond polls cond until it holds or the timeout elapses.
func waitForCond(t *testing.T, timeout time.Duration, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s", what)
}

// recvWithin fails the test if nothing is received on ch within the timeout.
func recvWithin(t *testing.T, timeout time.Duration, what string, ch <-chan struct{}) {
	t.Helper()
	select {
	case <-ch:
	case <-time.After(timeout):
		t.Fatalf("timed out waiting for %s", what)
	}
}

// trackedBody counts how many times Close is called on it.
type trackedBody struct {
	io.Reader
	mu     *sync.Mutex
	closes *int
}

func (b *trackedBody) Close() error {
	b.mu.Lock()
	*b.closes++
	b.mu.Unlock()
	return nil
}

// handledLog collects the URLs handed to the Handler, in call order.
type handledLog struct {
	mu   sync.Mutex
	urls []string
}

func (l *handledLog) add(u string) {
	l.mu.Lock()
	l.urls = append(l.urls, u)
	l.mu.Unlock()
}

func (l *handledLog) count(pred func(string) bool) int {
	l.mu.Lock()
	defer l.mu.Unlock()
	n := 0
	for _, u := range l.urls {
		if pred(u) {
			n++
		}
	}
	return n
}

func (l *handledLog) snapshot() []string {
	l.mu.Lock()
	defer l.mu.Unlock()
	return append([]string(nil), l.urls...)
}

// Many producers send while a control goroutine closes the queue. Every Send
// that reports success must be handled exactly once; sends that race with the
// close may fail with ErrQueueClosed, but a successful send must never be
// dropped, and no send may block forever or panic.
func TestQueueShutdownConcurrentProducersClose(t *testing.T) {
	var log handledLog
	f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		if err != nil {
			t.Errorf("unexpected handler error for %s: %v", ctx.Cmd.URL(), err)
			return
		}
		log.add(ctx.Cmd.URL().String())
	}))
	f.DisablePoliteness = true
	f.CrawlDelay = 0
	f.HttpClient = sdOK()
	q := f.Start()

	const producers = 8
	const perProducer = 25
	var sentMu sync.Mutex
	sent := make(map[string]bool)
	var pwg sync.WaitGroup
	for p := 0; p < producers; p++ {
		pwg.Add(1)
		go func(p int) {
			defer pwg.Done()
			for i := 0; i < perProducer; i++ {
				u := fmt.Sprintf("https://host-%d.invalid/item/%d/%d", p%3, p, i)
				if _, err := q.SendStringGet(u); err != nil {
					if err != ErrQueueClosed {
						t.Errorf("producer %d: unexpected send error: %v", p, err)
					}
					continue
				}
				sentMu.Lock()
				sent[u] = true
				sentMu.Unlock()
			}
		}(p)
	}

	// Make sure the queue is actually flowing before closing.
	waitForCond(t, 5*time.Second, "first command to be handled", func() bool {
		return log.count(func(string) bool { return true }) > 0
	})

	// Close from an independent control goroutine while producers run.
	closeDone := make(chan struct{})
	go func() {
		if err := q.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
		close(closeDone)
	}()
	pwg.Wait()
	recvWithin(t, 10*time.Second, "Close to drain the queue", closeDone)

	sentMu.Lock()
	defer sentMu.Unlock()
	if len(sent) == 0 {
		t.Fatal("no send succeeded, test is vacuous")
	}
	got := log.snapshot()
	if len(got) != len(sent) {
		t.Fatalf("handled %d commands, but %d sends succeeded", len(got), len(sent))
	}
	seen := make(map[string]int, len(got))
	for _, u := range got {
		seen[u]++
	}
	for u := range sent {
		if n := seen[u]; n != 1 {
			t.Errorf("command %s handled %d times, want exactly 1", u, n)
		}
	}
}

// Concurrent and repeated Close calls must all wait for the same completion
// barrier: none of them returns while an in-flight handler is still blocked,
// and Done/Block report that same point.
func TestQueueShutdownRepeatedCloseWaitsForDrain(t *testing.T) {
	entered := make(chan struct{})
	release := make(chan struct{})
	var once sync.Once
	f := New(HandlerFunc(func(*Context, *http.Response, error) {
		once.Do(func() { close(entered) })
		<-release
	}))
	f.DisablePoliteness = true
	f.CrawlDelay = 0
	f.HttpClient = sdOK()
	q := f.Start()
	if _, err := q.SendStringGet("https://one.invalid/a"); err != nil {
		t.Fatal(err)
	}
	<-entered

	const closers = 4
	returned := make(chan error, closers)
	for i := 0; i < closers; i++ {
		go func() { returned <- q.Close() }()
	}
	// Admission stops as soon as the first Close starts.
	waitForCond(t, 5*time.Second, "closed signal", func() bool {
		select {
		case <-q.closed:
			return true
		default:
			return false
		}
	})
	if _, err := q.SendStringGet("https://one.invalid/b"); err != ErrQueueClosed {
		t.Fatalf("expected ErrQueueClosed after Close started, got %v", err)
	}
	// No Close may return while the in-flight handler is still blocked.
	select {
	case err := <-returned:
		t.Fatalf("a Close returned (%v) before the in-flight handler completed", err)
	case <-time.After(100 * time.Millisecond):
	}
	select {
	case <-q.Done():
		t.Fatal("Done closed before the in-flight handler completed")
	default:
	}

	close(release)
	for i := 0; i < closers; i++ {
		if err := <-returned; err != nil {
			t.Fatalf("Close returned error: %v", err)
		}
	}
	<-q.Done()
	q.Block() // must not block anymore
	if q.Done() != q.Done() {
		t.Fatal("Done returned different channels on consecutive calls")
	}
	// A late Close returns promptly, without panic.
	if err := q.Close(); err != nil {
		t.Fatal(err)
	}
}

// A Cancel racing an in-progress Close upgrades it: commands still queued for
// the host are dropped without Handler calls, the in-flight one runs to
// completion, and both shutdown calls return through the same barrier.
func TestQueueShutdownCancelUpgradesClose(t *testing.T) {
	release := make(chan struct{})
	first := make(chan struct{})
	var once sync.Once
	var log handledLog
	f := New(HandlerFunc(func(ctx *Context, _ *http.Response, _ error) {
		log.add(ctx.Cmd.URL().Path)
		once.Do(func() { close(first) })
		<-release
	}))
	f.DisablePoliteness = true
	f.CrawlDelay = 0
	f.HttpClient = sdOK()
	q := f.Start()
	for _, p := range []string{"/1", "/2", "/3", "/4", "/5"} {
		if _, err := q.SendStringGet("https://one.invalid" + p); err != nil {
			t.Fatal(err)
		}
	}
	<-first

	closeDone := make(chan struct{})
	go func() {
		if err := q.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
		close(closeDone)
	}()
	// Wait for the normal close to stop admission, then upgrade it.
	waitForCond(t, 5*time.Second, "closed signal", func() bool {
		select {
		case <-q.closed:
			return true
		default:
			return false
		}
	})
	cancelDone := make(chan struct{})
	go func() {
		if err := q.Cancel(); err != nil {
			t.Errorf("Cancel: %v", err)
		}
		close(cancelDone)
	}()
	// Neither shutdown call returns while the in-flight handler is blocked.
	select {
	case <-closeDone:
		t.Fatal("Close returned before the in-flight handler completed")
	case <-cancelDone:
		t.Fatal("Cancel returned before the in-flight handler completed")
	case <-time.After(100 * time.Millisecond):
	}

	close(release)
	recvWithin(t, 10*time.Second, "Close to return", closeDone)
	recvWithin(t, 10*time.Second, "Cancel to return", cancelDone)
	<-q.Done()

	got := log.snapshot()
	if len(got) != 1 || got[0] != "/1" {
		t.Fatalf("expected only the in-flight command /1 to be handled, got %v", got)
	}
	// Mixed repeated shutdown calls after completion return promptly.
	if err := q.Cancel(); err != nil {
		t.Fatal(err)
	}
	if err := q.Close(); err != nil {
		t.Fatal(err)
	}
}

// Two hosts make progress independently: while one host is blocked inside
// HttpClient.Do, the other drains its whole backlog. Close waits for the
// blocked host, per-host FIFO order is preserved, every accepted command is
// handled exactly once, and each response body is closed exactly once.
func TestQueueShutdownTwoSitesBacklog(t *testing.T) {
	gateA := make(chan struct{})
	var bodyMu sync.Mutex
	bodyCloses := 0
	var log handledLog
	f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		if err != nil {
			t.Errorf("unexpected handler error for %s: %v", ctx.Cmd.URL(), err)
			return
		}
		log.add(ctx.Cmd.URL().String())
	}))
	f.DisablePoliteness = true
	f.CrawlDelay = 0
	f.HttpClient = stubDoer(func(r *http.Request) (*http.Response, error) {
		if r.URL.Host == "a.invalid" {
			<-gateA
		}
		return &http.Response{
			StatusCode: 200,
			Header:     make(http.Header),
			Body:       &trackedBody{Reader: strings.NewReader("ok"), mu: &bodyMu, closes: &bodyCloses},
			Request:    r,
		}, nil
	})
	q := f.Start()

	const perHost = 20
	sentA, sentB := make([]string, 0, perHost), make([]string, 0, perHost)
	for i := 0; i < perHost; i++ {
		ua := fmt.Sprintf("https://a.invalid/%03d", i)
		ub := fmt.Sprintf("https://b.invalid/%03d", i)
		if _, err := q.SendStringGet(ua); err != nil {
			t.Fatal(err)
		}
		sentA = append(sentA, ua)
		if _, err := q.SendStringGet(ub); err != nil {
			t.Fatal(err)
		}
		sentB = append(sentB, ub)
	}

	isB := func(u string) bool { return strings.Contains(u, "b.invalid") }
	isA := func(u string) bool { return strings.Contains(u, "a.invalid") }
	// Host B drains completely while host A is still blocked inside Do.
	waitForCond(t, 10*time.Second, "host B backlog to drain", func() bool {
		return log.count(isB) == perHost
	})
	if n := log.count(isA); n != 0 {
		t.Fatalf("host A handled %d commands while its Doer was blocked, want 0", n)
	}

	// Close must wait for the blocked host too.
	closeDone := make(chan struct{})
	go func() {
		if err := q.Close(); err != nil {
			t.Errorf("Close: %v", err)
		}
		close(closeDone)
	}()
	select {
	case <-closeDone:
		t.Fatal("Close returned while a host was still blocked inside Do")
	case <-time.After(100 * time.Millisecond):
	}
	close(gateA)
	recvWithin(t, 10*time.Second, "Close to drain both hosts", closeDone)
	<-q.Done()

	// Per-host FIFO order, and the handled set matches the sent set exactly.
	gotA, gotB := []string{}, []string{}
	for _, u := range log.snapshot() {
		if isA(u) {
			gotA = append(gotA, u)
		} else {
			gotB = append(gotB, u)
		}
	}
	if fmt.Sprint(gotA) != fmt.Sprint(sentA) {
		t.Fatalf("host A handled order\n got: %v\nwant: %v", gotA, sentA)
	}
	if fmt.Sprint(gotB) != fmt.Sprint(sentB) {
		t.Fatalf("host B handled order\n got: %v\nwant: %v", gotB, sentB)
	}
	// Each response body was closed exactly once by the fetcher.
	bodyMu.Lock()
	if bodyCloses != 2*perHost {
		t.Fatalf("response bodies closed %d times, want %d", bodyCloses, 2*perHost)
	}
	bodyMu.Unlock()
}

// A worker parked on a long crawl delay must react to Cancel promptly instead
// of waiting out the full politeness interval. The command already handled
// stays handled; the one waiting behind the delay is dropped.
func TestQueueShutdownCancelInterruptsCrawlDelay(t *testing.T) {
	var log handledLog
	f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		log.add(ctx.Cmd.URL().Path)
	}))
	f.DisablePoliteness = true
	f.CrawlDelay = time.Hour // deliberately huge: the test must not wait it out
	f.HttpClient = sdOK()
	q := f.Start()
	for _, p := range []string{"/1", "/2"} {
		if _, err := q.SendStringGet("https://slow.invalid" + p); err != nil {
			t.Fatal(err)
		}
	}
	waitForCond(t, 5*time.Second, "first command to be handled", func() bool {
		return log.count(func(string) bool { return true }) == 1
	})

	// The worker is now parked on the hour-long delay before /2.
	cancelDone := make(chan struct{})
	go func() {
		if err := q.Cancel(); err != nil {
			t.Errorf("Cancel: %v", err)
		}
		close(cancelDone)
	}()
	recvWithin(t, 10*time.Second, "Cancel to interrupt the crawl-delay wait", cancelDone)
	<-q.Done()

	got := log.snapshot()
	if len(got) != 1 || got[0] != "/1" {
		t.Fatalf("expected only /1 to be handled, got %v", got)
	}
	// Repeated mixed shutdown calls return promptly.
	if err := q.Close(); err != nil {
		t.Fatal(err)
	}
	if err := q.Cancel(); err != nil {
		t.Fatal(err)
	}
}

// Same as the crawl-delay case, but the wait comes from the robots.txt
// crawl-delay: Cancel must interrupt that wait too.
func TestQueueShutdownCancelInterruptsRobotsDelay(t *testing.T) {
	robotsFetched := make(chan struct{})
	var robotsOnce sync.Once
	var log handledLog
	f := New(HandlerFunc(func(ctx *Context, res *http.Response, err error) {
		log.add(ctx.Cmd.URL().Path)
	}))
	f.HttpClient = stubDoer(func(r *http.Request) (*http.Response, error) {
		body := "ok"
		if r.URL.Path == "/robots.txt" {
			body = "User-agent: *\nCrawl-delay: 3600\n"
			robotsOnce.Do(func() { close(robotsFetched) })
		}
		return &http.Response{StatusCode: 200, Header: make(http.Header), Body: io.NopCloser(strings.NewReader(body)), Request: r}, nil
	})
	q := f.Start()
	if _, err := q.SendStringGet("https://polite.invalid/a"); err != nil {
		t.Fatal(err)
	}
	recvWithin(t, 5*time.Second, "robots.txt to be fetched", robotsFetched)
	// Give the worker a moment to park on the robots crawl-delay.
	time.Sleep(50 * time.Millisecond)

	cancelDone := make(chan struct{})
	go func() {
		if err := q.Cancel(); err != nil {
			t.Errorf("Cancel: %v", err)
		}
		close(cancelDone)
	}()
	recvWithin(t, 10*time.Second, "Cancel to interrupt the robots crawl-delay wait", cancelDone)
	<-q.Done()

	if got := log.snapshot(); len(got) != 0 {
		t.Fatalf("expected no command to be handled, got %v", got)
	}
}
