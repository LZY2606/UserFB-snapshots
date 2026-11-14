package crawler

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"testing"
	"time"

	"github.com/0xfurai/goliath/internal/storage"
	"github.com/0xfurai/goliath/task_support/taskcheck"
)

// errOutputSentinel is the synthetic storage failure injected in crawler
// tests; identity equality makes errors.Is assertions unambiguous.
var (
	errOutputSentinel = errors.New("synthetic output failure")
	errCloseSentinel  = errors.New("synthetic close failure")
)

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

// failingWriter wraps a real *storage.Writer and fails the failAt-th Write
// with errOutputSentinel. Successful writes still land on disk, so tests
// can assert which records survived before the failure. When closeErr is
// set, Close reports it after still closing the real file, exercising the
// final-close failure path.
type failingWriter struct {
	inner    *storage.Writer
	mu       sync.Mutex
	calls    int
	failAt   int
	closeErr error
	failed   bool
	urls     []string
}

func (w *failingWriter) Write(r storage.Record) error {
	w.mu.Lock()
	w.calls++
	trigger := !w.failed && w.calls == w.failAt
	if trigger {
		w.failed = true
	}
	w.mu.Unlock()

	if trigger {
		return errOutputSentinel
	}
	if err := w.inner.Write(r); err != nil {
		return err
	}
	w.mu.Lock()
	w.urls = append(w.urls, r.URL)
	w.mu.Unlock()
	return nil
}

func (w *failingWriter) Close() error {
	_ = w.inner.Close()
	return w.closeErr
}
func (w *failingWriter) Err() error { return w.inner.Err() }

func (w *failingWriter) snapshot() (calls int, urls []string) {
	w.mu.Lock()
	defer w.mu.Unlock()
	urls = append([]string(nil), w.urls...)
	return w.calls, urls
}

// scriptedTransport is a fully controllable http.RoundTripper: per-path
// response gates, transport errors, content types, links and in-flight
// observation channels.
type scriptedTransport struct {
	mu       sync.Mutex
	hits     map[string]int
	entered  map[string]chan struct{}
	gates    map[string]chan struct{}
	rtErrs   map[string]error
	plain    map[string]bool
	links    map[string][]string
	canceled map[string]bool
}

func newScriptedTransport() *scriptedTransport {
	return &scriptedTransport{
		hits:     map[string]int{},
		entered:  map[string]chan struct{}{},
		gates:    map[string]chan struct{}{},
		rtErrs:   map[string]error{},
		plain:    map[string]bool{},
		links:    map[string][]string{},
		canceled: map[string]bool{},
	}
}

func (t *scriptedTransport) watch(path string) chan struct{} {
	ch := make(chan struct{}, 1)
	t.entered[path] = ch
	return ch
}

func (t *scriptedTransport) hitCount(path string) int {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.hits[path]
}

func (t *scriptedTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	if req.URL.Path == "/robots.txt" {
		body := "User-agent: *\nAllow: /\n"
		return &http.Response{
			StatusCode: 200,
			Header:     http.Header{"Content-Type": {"text/plain"}},
			Body:       io.NopCloser(strings.NewReader(body)),
			Request:    req,
		}, nil
	}

	path := req.URL.Path
	t.mu.Lock()
	t.hits[path]++
	if ch := t.entered[path]; ch != nil {
		select {
		case ch <- struct{}{}:
		default:
		}
	}
	gate := t.gates[path]
	rtErr := t.rtErrs[path]
	isPlain := t.plain[path]
	links := t.links[path]
	t.mu.Unlock()

	if gate != nil {
		select {
		case <-gate:
		case <-req.Context().Done():
			t.mu.Lock()
			t.canceled[path] = true
			t.mu.Unlock()
			return nil, req.Context().Err()
		}
	}
	if rtErr != nil {
		return nil, rtErr
	}

	contentType := "text/html; charset=utf-8"
	var body string
	if isPlain {
		contentType = "text/plain"
		body = "plain binary body"
	} else {
		var sb strings.Builder
		sb.WriteString(`<html><head><title>` + path + `</title></head><body>`)
		for _, l := range links {
			sb.WriteString(`<a href="` + l + `">x</a>`)
		}
		sb.WriteString(`</body></html>`)
		body = sb.String()
	}
	return &http.Response{
		StatusCode: 200,
		Header:     http.Header{"Content-Type": {contentType}},
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    req,
	}, nil
}

func newOutputCrawler(t *testing.T, tr *scriptedTransport, seeds []string, workers int, minDelay time.Duration) (*Crawler, string) {
	t.Helper()
	out := filepath.Join(t.TempDir(), "pages.jsonl")
	c, err := New(Config{
		Seeds:      seeds,
		Workers:    workers,
		MaxDepth:   1,
		MinDelay:   minDelay,
		Timeout:    10 * time.Second,
		OutputFile: out,
	}, slog.New(slog.NewTextHandler(io.Discard, nil)))
	if err != nil {
		t.Fatal(err)
	}
	c.fetcher.Client().Transport = tr
	return c, out
}

func installFailingWriter(c *Crawler, failAt int) *failingWriter {
	fw := &failingWriter{inner: c.writer.(*storage.Writer), failAt: failAt}
	c.writer = fw
	return fw
}

// runWithGuard runs the crawler, failing the test if it does not drain
// within the timeout. It must return promptly on every shutdown path.
func runWithGuard(t *testing.T, c *Crawler, ctx context.Context, timeout time.Duration) StatsSnapshot {
	t.Helper()
	return awaitRun(t, runAsync(c, ctx), timeout)
}

// runAsync starts Run in the background and returns a result channel.
func runAsync(c *Crawler, ctx context.Context) <-chan StatsSnapshot {
	done := make(chan StatsSnapshot, 1)
	go func() { done <- c.Run(ctx) }()
	return done
}

// awaitRun waits for a background Run, failing on timeout.
func awaitRun(t *testing.T, done <-chan StatsSnapshot, timeout time.Duration) StatsSnapshot {
	t.Helper()
	select {
	case s := <-done:
		return s
	case <-time.After(timeout):
		t.Fatalf("crawler Run did not return within %s (drain broken)", timeout)
		return StatsSnapshot{}
	}
}

// readJSONL returns the raw lines and parsed records of a JSONL file.
func readJSONL(t *testing.T, path string) ([][]byte, []storage.Record, error) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, nil, err
	}
	var lines [][]byte
	var recs []storage.Record
	for _, line := range bytes.Split(data, []byte("\n")) {
		if len(line) == 0 {
			continue
		}
		lines = append(lines, line)
		var r storage.Record
		if err := json.Unmarshal(line, &r); err != nil {
			return lines, recs, fmt.Errorf("invalid JSON line %q: %w", string(line), err)
		}
		recs = append(recs, r)
	}
	return lines, recs, nil
}

func waitEntered(t *testing.T, tr *scriptedTransport, path string, timeout time.Duration) {
	t.Helper()
	tr.mu.Lock()
	ch := tr.entered[path]
	tr.mu.Unlock()
	if ch == nil {
		t.Fatalf("no observation channel for %s", path)
	}
	select {
	case <-ch:
	case <-time.After(timeout):
		t.Fatalf("request %s never entered transport", path)
	}
}

// ---------------------------------------------------------------------------
// Group 1: real short writes via a subprocess file-size limit
// ---------------------------------------------------------------------------

const (
	fsizeChildEnv   = "GOLIATH_FSIZE_CHILD"
	fsizePathEnv    = "GOLIATH_FSIZE_PATH"
	fsizeChildOK    = "FSIZE_CHILD_OK"
	fsizeLimitExtra = 16 // bytes the kernel may accept past the prefix
)

// runFSIZEChild executes in a re-execed test binary. It raises
// RLIMIT_FSIZE so the kernel accepts only part of a record, then verifies
// that storage.Writer rolls the partial line back and refuses later writes.
// It never returns: the process exits with the check status.
func runFSIZEChild() {
	path := os.Getenv(fsizePathEnv)
	extra := fsizeLimitExtra

	sigCh := make(chan os.Signal, 16)
	signal.Notify(sigCh, syscall.SIGXFSZ)
	go func() {
		for range sigCh {
		}
	}()

	fail := func(msg string) {
		fmt.Println("CHILD_FAILURE: " + msg)
		os.Exit(1)
	}
	mark := func(name string, ok bool) {
		fmt.Printf("%s=%t\n", name, ok)
		if !ok {
			fail(name)
		}
	}

	w, err := storage.NewWriter(path)
	if err != nil {
		fail("open: " + err.Error())
	}
	info, err := os.Stat(path)
	if err != nil {
		fail("stat: " + err.Error())
	}
	start := info.Size()

	if err := syscall.Setrlimit(syscall.RLIMIT_FSIZE, &syscall.Rlimit{
		Cur: uint64(start) + uint64(extra),
		Max: uint64(start) + uint64(extra),
	}); err != nil {
		fail("setrlimit: " + err.Error())
	}

	longRecord := storage.Record{
		URL:       "https://fsize.example.invalid/long",
		Title:     "long",
		Content:   strings.Repeat("x", 300),
		Links:     []string{},
		FetchedAt: time.Unix(1700000000, 0).UTC(),
		Depth:     1,
	}
	firstErr := w.Write(longRecord)
	mark("CHILD_FIRST_WRITE_ERR", firstErr != nil)
	mark("CHILD_FIRST_EFBIG", errors.Is(firstErr, syscall.EFBIG))

	info, _ = os.Stat(path)
	mark("CHILD_SIZE_RESTORED", info.Size() == start)

	// Concurrent writes after the first failure must all be rejected and
	// must not move the file.
	var wg sync.WaitGroup
	var rejected int32
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			err := w.Write(storage.Record{
				URL:       fmt.Sprintf("https://fsize.example.invalid/%d", i),
				FetchedAt: time.Unix(1700000000, 0).UTC(),
			})
			if err != nil {
				atomic.AddInt32(&rejected, 1)
			}
		}(i)
	}
	wg.Wait()
	mark("CHILD_CONCURRENT_REJECTED", rejected == 8)

	info, _ = os.Stat(path)
	mark("CHILD_SIZE_STILL_START", info.Size() == start)
	mark("CHILD_ERR_EFBIG", errors.Is(w.Err(), syscall.EFBIG))

	if err := w.Close(); err != nil {
		fail("close: " + err.Error())
	}
	fmt.Println(fsizeChildOK)
	os.Exit(0)
}

func TestOutputRecordBoundary(t *testing.T) {
	if os.Getenv(fsizeChildEnv) == "1" {
		runFSIZEChild()
	}

	taskcheck.Run(t, func(c *taskcheck.C) {
		dir := t.TempDir()
		path := filepath.Join(dir, "pages.jsonl")

		// Seed an existing, complete record; its bytes must survive intact.
		prefixWriter, err := storage.NewWriter(path)
		if err != nil {
			t.Fatal(err)
		}
		prefixRecord := storage.Record{
			URL:       "https://prefix.example.invalid/seed",
			Title:     "prefix",
			Links:     []string{},
			FetchedAt: time.Unix(1700000000, 0).UTC(),
		}
		if err := prefixWriter.Write(prefixRecord); err != nil {
			t.Fatal(err)
		}
		if err := prefixWriter.Close(); err != nil {
			t.Fatal(err)
		}
		prefixBytes, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		prefixInfo, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}

		// Re-exec the test binary with a file size rlimit in force.
		cmd := exec.Command(os.Args[0], "-test.run=^TestOutputRecordBoundary$")
		cmd.Env = append(os.Environ(),
			fsizeChildEnv+"=1",
			fsizePathEnv+"="+path,
		)
		out, cmdErr := cmd.CombinedOutput()
		childOut := string(out)

		c.Check(cmdErr == nil, "subprocess exited with error: %v\n%s", cmdErr, childOut)
		for _, marker := range []string{
			"CHILD_FIRST_WRITE_ERR=true",
			"CHILD_FIRST_EFBIG=true",
			"CHILD_SIZE_RESTORED=true",
			"CHILD_CONCURRENT_REJECTED=true",
			"CHILD_SIZE_STILL_START=true",
			"CHILD_ERR_EFBIG=true",
			fsizeChildOK,
		} {
			c.Check(strings.Contains(childOut, marker), "child output missing %q:\n%s", marker, childOut)
		}

		// Parent-side verification: the pre-existing prefix is byte-for-byte
		// unchanged and is still the only, complete JSON line.
		after, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(bytes.Equal(after, prefixBytes), "existing prefix bytes changed:\nbefore=%x\nafter =%x", prefixBytes, after)

		info, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		c.Check(info.Size() == prefixInfo.Size(), "file size changed: prefix=%d now=%d", prefixInfo.Size(), info.Size())

		lines, recs, err := readJSONL(t, path)
		c.Check(err == nil, "prefix file no longer parseable: %v", err)
		c.Check(len(lines) == 1 && len(recs) == 1, "expected exactly one intact line, got %d", len(lines))
		if len(recs) == 1 {
			c.Check(recs[0].URL == prefixRecord.URL, "surviving record is not the prefix: %+v", recs[0])
		}

		// A fresh writer can append a complete line afterward.
		next, err := storage.NewWriter(path)
		if err != nil {
			t.Fatal(err)
		}
		appendRec := storage.Record{URL: "https://prefix.example.invalid/next", FetchedAt: time.Unix(1700000001, 0).UTC()}
		c.Check(next.Write(appendRec) == nil, "write after rollback should succeed")
		c.Check(next.Close() == nil, "close after rollback should succeed")
		lines, recs, err = readJSONL(t, path)
		c.Check(err == nil && len(lines) == 2 && len(recs) == 2, "expected two intact lines, got %d err=%v", len(lines), err)
	})
}

// ---------------------------------------------------------------------------
// Group 2: first output failure stops admission on all three write paths
// ---------------------------------------------------------------------------

func TestOutputStopAdmission(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- HTML write path: the failed page is not fetched and its
		// discovered links are never enqueued or requested. ---
		func() {
			tr := newScriptedTransport()
			tr.links["/page-a"] = []string{"/child"}
			tr.watch("/page-a")
			cr, out := newOutputCrawler(t, tr,
				[]string{"https://stop.example.invalid/page-a", "https://stop.example.invalid/later"}, 1, time.Nanosecond)
			fw := installFailingWriter(cr, 1)

			done := runAsync(cr, context.Background())
			waitEntered(t, tr, "/page-a", 3*time.Second)
			stats := awaitRun(t, done, 5*time.Second)

			c.Check(errors.Is(cr.Err(), errOutputSentinel), "HTML path Err should wrap sentinel, got %v", cr.Err())
			c.Check(stats.Fetched == 0, "failed HTML write must not count Fetched, got %d", stats.Fetched)
			c.Check(tr.hitCount("/page-a") == 1, "page-a should be requested once, got %d", tr.hitCount("/page-a"))
			c.Check(tr.hitCount("/child") == 0, "child link must never be requested, got %d", tr.hitCount("/child"))
			c.Check(tr.hitCount("/later") == 0, "queued page admitted after failure: %d", tr.hitCount("/later"))
			lines, _, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 0, "no complete records expected, got %d lines err=%v", len(lines), err)
			calls, urls := fw.snapshot()
			c.Check(calls == 1 && len(urls) == 0, "only the failed write should have happened, calls=%d urls=%v", calls, urls)

			size1, _ := os.Stat(out)
			time.Sleep(50 * time.Millisecond)
			size2, _ := os.Stat(out)
			c.Check(size1.Size() == size2.Size(), "file changed after Run returned")
		}()

		// --- Non-HTML write path. ---
		func() {
			tr := newScriptedTransport()
			tr.plain["/plain"] = true
			tr.watch("/plain")
			cr, out := newOutputCrawler(t, tr, []string{"https://stop.example.invalid/plain"}, 1, time.Nanosecond)
			installFailingWriter(cr, 1)

			done := runAsync(cr, context.Background())
			waitEntered(t, tr, "/plain", 3*time.Second)
			stats := awaitRun(t, done, 5*time.Second)

			c.Check(errors.Is(cr.Err(), errOutputSentinel), "non-HTML Err should wrap sentinel, got %v", cr.Err())
			c.Check(stats.Fetched == 0, "failed non-HTML write must not count Fetched, got %d", stats.Fetched)
			c.Check(tr.hitCount("/plain") == 1, "plain page requested once, got %d", tr.hitCount("/plain"))
			lines, _, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 0, "no records expected on failed non-HTML write, got %d", len(lines))
		}()

		// --- Transport-failure record write path. ---
		func() {
			tr := newScriptedTransport()
			netErr := errors.New("synthetic transport failure")
			tr.rtErrs["/broken"] = netErr
			tr.watch("/broken")
			cr, out := newOutputCrawler(t, tr, []string{"https://stop.example.invalid/broken"}, 1, time.Nanosecond)
			installFailingWriter(cr, 1)

			done := runAsync(cr, context.Background())
			waitEntered(t, tr, "/broken", 3*time.Second)
			stats := awaitRun(t, done, 5*time.Second)

			c.Check(errors.Is(cr.Err(), errOutputSentinel), "error-record Err should wrap sentinel, got %v", cr.Err())
			c.Check(stats.Errors == 1, "network failure still counted as Errors, got %d", stats.Errors)
			c.Check(stats.Fetched == 0, "transport failure must not count Fetched, got %d", stats.Fetched)
			lines, _, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 0, "failed error record must not reach disk, got %d", len(lines))
		}()

		// --- Control: a normal network error is still recorded when the
		// writer is healthy, and Err stays nil. ---
		func() {
			tr := newScriptedTransport()
			tr.rtErrs["/broken-ok"] = errors.New("ordinary network outage")
			cr, out := newOutputCrawler(t, tr, []string{"https://stop.example.invalid/broken-ok"}, 1, time.Nanosecond)
			stats := runWithGuard(t, cr, context.Background(), 5*time.Second)

			c.Check(cr.Err() == nil, "healthy run must report nil Err, got %v", cr.Err())
			c.Check(stats.Errors == 1 && stats.Fetched == 0, "expected Errors=1 Fetched=0, got %+v", stats)
			lines, recs, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 1, "one error record expected, got %d err=%v", len(lines), err)
			if len(recs) == 1 {
				c.Check(recs[0].Error != "", "error record must carry the error text")
			}
		}()

		// --- A failure only at Close is still reported via Err. ---
		func() {
			tr := newScriptedTransport()
			tr.watch("/close-only")
			cr, out := newOutputCrawler(t, tr, []string{"https://stop.example.invalid/close-only"}, 1, time.Nanosecond)
			fw := installFailingWriter(cr, 0) // failAt 0 => writes never fail
			fw.closeErr = errCloseSentinel

			stats := runWithGuard(t, cr, context.Background(), 5*time.Second)

			c.Check(errors.Is(cr.Err(), errCloseSentinel), "lone close failure must surface via Err, got %v", cr.Err())
			c.Check(stats.Fetched == 1, "records written before close still count, got %d", stats.Fetched)
			lines, _, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 1, "the completed record stays intact, got %d err=%v", len(lines), err)
		}()

		// --- Close failure after a write failure keeps both causes, with
		// the primary write error still reachable. ---
		func() {
			tr := newScriptedTransport()
			tr.watch("/two-faults")
			cr, _ := newOutputCrawler(t, tr, []string{"https://stop.example.invalid/two-faults"}, 1, time.Nanosecond)
			fw := installFailingWriter(cr, 1) // first write fails
			fw.closeErr = errCloseSentinel

			runWithGuard(t, cr, context.Background(), 5*time.Second)
			err := cr.Err()

			c.Check(errors.Is(err, errOutputSentinel), "primary write error lost, got %v", err)
			c.Check(errors.Is(err, errCloseSentinel), "close error not preserved, got %v", err)
		}()
	})
}

// ---------------------------------------------------------------------------
// Group 3: drain of waiting/in-flight workers and healthy parallel control
// ---------------------------------------------------------------------------

func TestOutputFailureDrain(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// --- A worker parked in the politeness delay is interrupted by the
		// first output failure instead of sleeping through MinDelay. ---
		func() {
			tr := newScriptedTransport()
			tr.plain["/fail"] = true
			tr.gates["/fail"] = make(chan struct{})
			tr.watch("/fail")
			tr.links["/a"] = []string{"/b"}
			tr.watch("/a")

			cr, out := newOutputCrawler(t, tr,
				[]string{"https://h1.example.invalid/a", "https://h2.example.invalid/fail"}, 2, 8*time.Second)
			installFailingWriter(cr, 2) // /a lands, then /fail's record fails

			done := runAsync(cr, context.Background())
			waitEntered(t, tr, "/fail", 3*time.Second)
			waitEntered(t, tr, "/a", 3*time.Second)
			// Give the /a worker time to pop /b and enter the long delay.
			time.Sleep(200 * time.Millisecond)

			start := time.Now()
			close(tr.gates["/fail"])
			stats := awaitRun(t, done, 5*time.Second)
			elapsed := time.Since(start)

			c.Check(elapsed < 4*time.Second, "waiting worker slept on (elapsed=%s, MinDelay=8s)", elapsed)
			c.Check(errors.Is(cr.Err(), errOutputSentinel), "drain Err should wrap sentinel, got %v", cr.Err())
			c.Check(stats.Fetched == 1, "only the fully written /a page counts, got %d", stats.Fetched)
			c.Check(tr.hitCount("/a") == 1, "/a requested once, got %d", tr.hitCount("/a"))
			c.Check(tr.hitCount("/b") == 0, "waiting worker must not start /b after failure, got %d", tr.hitCount("/b"))
			c.Check(tr.hitCount("/fail") == 1, "/fail requested once, got %d", tr.hitCount("/fail"))
			lines, recs, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 1, "only /a should survive, got %d lines err=%v", len(lines), err)
			if len(recs) == 1 {
				c.Check(strings.HasSuffix(recs[0].URL, "/a"), "survivor should be /a, got %s", recs[0].URL)
			}
		}()

		// --- A worker blocked inside an HTTP request is canceled and must
		// exit; nothing is written after Run returns. ---
		func() {
			tr := newScriptedTransport()
			tr.gates["/slow"] = make(chan struct{})
			tr.watch("/slow")
			tr.plain["/quick"] = true
			tr.watch("/quick")

			cr, out := newOutputCrawler(t, tr,
				[]string{"https://drain.example.invalid/slow", "https://drain.example.invalid/quick"}, 2, time.Nanosecond)
			installFailingWriter(cr, 1) // /quick's record triggers the failure

			done := runAsync(cr, context.Background())
			waitEntered(t, tr, "/slow", 3*time.Second)
			waitEntered(t, tr, "/quick", 3*time.Second)
			stats := awaitRun(t, done, 5*time.Second)

			tr.mu.Lock()
			slowCanceled := tr.canceled["/slow"]
			tr.mu.Unlock()
			c.Check(slowCanceled, "in-flight /slow request context was not canceled")
			c.Check(errors.Is(cr.Err(), errOutputSentinel), "in-flight drain Err should wrap sentinel, got %v", cr.Err())
			c.Check(stats.Fetched == 0, "nothing counts Fetched when the only write fails, got %d", stats.Fetched)
			lines, _, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 0, "no records should exist, got %d", len(lines))
			size1, _ := os.Stat(out)
			time.Sleep(50 * time.Millisecond)
			size2, _ := os.Stat(out)
			c.Check(size1.Size() == size2.Size(), "file changed after Run returned")
		}()

		// --- Output failure racing external cancellation: the file error
		// must not be overwritten by context.Canceled. ---
		func() {
			tr := newScriptedTransport()
			tr.links["/race"] = []string{"/race-child"}
			tr.watch("/race")
			cr, _ := newOutputCrawler(t, tr, []string{"https://race.example.invalid/race"}, 1, time.Nanosecond)
			installFailingWriter(cr, 1)

			ctx, cancel := context.WithCancel(context.Background())
			done := runAsync(cr, ctx)
			waitEntered(t, tr, "/race", 3*time.Second)
			go func() {
				time.Sleep(20 * time.Millisecond)
				cancel()
			}()
			awaitRun(t, done, 5*time.Second)

			err := cr.Err()
			c.Check(err != nil, "race run must report the output error")
			c.Check(errors.Is(err, errOutputSentinel), "file error lost under concurrent cancel, got %v", err)
		}()

		// --- Healthy parallel control: Workers concurrency, depth limit,
		// URL deduplication and clean shutdown all keep working. ---
		func() {
			tr := newScriptedTransport()
			release := make(chan struct{})
			tr.gates["/root"] = release
			tr.gates["/sibling"] = release
			tr.watch("/root")
			tr.watch("/sibling")
			tr.links["/root"] = []string{"/child", "/child"}
			tr.links["/child"] = []string{"/deep"}
			tr.watch("/child")

			enteredRoot := tr.entered["/root"]
			enteredSibling := tr.entered["/sibling"]

			cr, out := newOutputCrawler(t, tr,
				[]string{"https://healthy.example.invalid/root", "https://healthy.example.invalid/sibling"}, 2, time.Nanosecond)

			done := make(chan StatsSnapshot, 1)
			go func() { done <- cr.Run(context.Background()) }()

			select {
			case <-enteredRoot:
			case <-time.After(3 * time.Second):
				t.Fatal("/root never started")
			}
			select {
			case <-enteredSibling:
			case <-time.After(3 * time.Second):
				t.Fatal("/sibling never started concurrently")
			}
			close(release)

			var stats StatsSnapshot
			select {
			case stats = <-done:
			case <-time.After(5 * time.Second):
				t.Fatal("healthy crawl did not finish (watchdog/Pop drain broken)")
			}

			c.Check(cr.Err() == nil, "healthy run Err must be nil, got %v", cr.Err())
			c.Check(stats.Fetched == 3, "root, sibling and deduped child = 3 fetched, got %d", stats.Fetched)
			c.Check(stats.Enqueued == 3, "2 seeds + 1 unique child enqueued, got %d", stats.Enqueued)
			c.Check(tr.hitCount("/root") == 1, "root hits=%d", tr.hitCount("/root"))
			c.Check(tr.hitCount("/sibling") == 1, "sibling hits=%d", tr.hitCount("/sibling"))
			c.Check(tr.hitCount("/child") == 1, "duplicate /child must dedupe, hits=%d", tr.hitCount("/child"))
			c.Check(tr.hitCount("/deep") == 0, "depth-2 /deep must be blocked by MaxDepth, hits=%d", tr.hitCount("/deep"))
			lines, recs, err := readJSONL(t, out)
			c.Check(err == nil && len(lines) == 3 && len(recs) == 3, "three intact records expected, got %d err=%v", len(lines), err)
		}()
	})
}
