package grab

import (
	"bytes"
	"context"
	"crypto/md5"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/cavaliergopher/grab/v3/task_support/taskcheck"
)

// liveWait bounds every blocking observation in these tests. It is only a
// deadlock tripwire; progress itself is driven through explicit gates, never
// through polling storage or sleeping.
const liveWait = 5 * time.Second

// errLiveNet and errLiveHook are sentinels so errors.Is can identify the
// original download error on a stream.
var (
	errLiveNet  = errors.New("live contract: simulated network failure")
	errLiveHook = errors.New("live contract: simulated AfterCopy failure")
)

// liveGate is a RateLimiter that parks the transfer goroutine after every
// published chunk. The first WaitN closes prefix, giving tests a deterministic
// "first chunk is visible" signal; every send on release admits one more
// limiter wait (and therefore one more transfer loop iteration).
type liveGate struct {
	prefix  chan struct{}
	release chan struct{}
	once    sync.Once
}

func newLiveGate() *liveGate {
	return &liveGate{prefix: make(chan struct{}), release: make(chan struct{})}
}

func (g *liveGate) WaitN(ctx context.Context, _ int) error {
	g.once.Do(func() { close(g.prefix) })
	select {
	case <-g.release:
		return nil
	case <-ctx.Done():
		return ctx.Err()
	}
}

// step releases one parked limiter wait. A transfer that performs k successful
// writes needs k steps after prefix has fired to run to its terminal state.
func (g *liveGate) step(t *testing.T) {
	t.Helper()
	select {
	case g.release <- struct{}{}:
	case <-time.After(liveWait):
		t.Fatal("transfer never reached the next gate")
	}
}

// chunkedBody hands out at most chunk bytes per Read, like a socket delivering
// small segments, and records Close for the body-lifecycle assertions.
type chunkedBody struct {
	data   []byte
	off    int
	chunk  int
	closed *int32
}

func (b *chunkedBody) Read(p []byte) (int, error) {
	if b.off >= len(b.data) {
		return 0, io.EOF
	}
	if len(p) > b.chunk {
		p = p[:b.chunk]
	}
	n := copy(p, b.data[b.off:])
	b.off += n
	return n, nil
}

func (b *chunkedBody) Close() error {
	atomic.AddInt32(b.closed, 1)
	return nil
}

// failingBody serves failAt bytes and then fails. If withData is true, the read
// that reaches the limit returns the remaining bytes and the error together,
// exercising the data-plus-error Read path.
type failingBody struct {
	data     []byte
	off      int
	chunk    int
	failAt   int
	withData bool
	closed   *int32
}

func (b *failingBody) Read(p []byte) (int, error) {
	if b.off >= b.failAt {
		return 0, errLiveNet
	}
	// Never hand over bytes past the failure point, even when a larger chunk
	// would fit: the failing read returns only the bytes up to failAt.
	if limit := b.failAt - b.off; len(p) > limit {
		p = p[:limit]
	}
	if len(p) > b.chunk {
		p = p[:b.chunk]
	}
	n := copy(p, b.data[b.off:])
	b.off += n
	if b.withData && b.off >= b.failAt {
		return n, errLiveNet
	}
	return n, nil
}

func (b *failingBody) Close() error {
	atomic.AddInt32(b.closed, 1)
	return nil
}

// liveTransport is an in-memory HTTPClient. HEAD advertises range support and
// the true length; GET honors a Range header so real resume flows are exercised
// without binding a loopback socket.
type liveTransport struct {
	payload   []byte
	chunk     int
	failAt    int // < 0 disables failure injection
	withData  bool
	rangeSeen chan int64
	closes    int32
}

func (t *liveTransport) Do(req *http.Request) (*http.Response, error) {
	header := http.Header{}
	header.Set("Accept-Ranges", "bytes")
	if req.Method == http.MethodHead {
		return &http.Response{
			StatusCode:    http.StatusOK,
			Header:        header,
			ContentLength: int64(len(t.payload)),
			Body:          http.NoBody,
			Request:       req,
		}, nil
	}

	start := int64(0)
	if rng := req.Header.Get("Range"); rng != "" {
		if _, err := fmt.Sscanf(rng, "bytes=%d-", &start); err != nil {
			return nil, err
		}
		select {
		case t.rangeSeen <- start:
		default:
		}
	}
	content := t.payload[start:]
	var body io.ReadCloser
	if t.failAt >= 0 {
		body = &failingBody{
			data:     content,
			chunk:    t.chunk,
			failAt:   t.failAt,
			withData: t.withData,
			closed:   &t.closes,
		}
	} else {
		body = &chunkedBody{
			data:   content,
			chunk:  t.chunk,
			closed: &t.closes,
		}
	}
	return &http.Response{
		StatusCode:    http.StatusOK,
		Header:        header,
		ContentLength: int64(len(content)),
		Body:          body,
		Request:       req,
	}, nil
}

func newLiveClient(tr *liveTransport) *Client {
	cl := NewClient()
	cl.HTTPClient = tr
	return cl
}

// errorTransport fails every HTTP request, modeling a connection that never
// produces a response or any storage.
type errorTransport struct{ err error }

func (t errorTransport) Do(*http.Request) (*http.Response, error) {
	return nil, t.err
}

func newLiveRequest(t *testing.T, dst string) *Request {
	t.Helper()
	req, err := NewRequest(dst, "http://live-contract.invalid/export")
	if err != nil {
		t.Fatal(err)
	}
	return req
}

// readResult is delivered by the asyncRead probe.
type readResult struct {
	n   int
	err error
}

func asyncRead(r io.Reader, buf []byte) <-chan readResult {
	ch := make(chan readResult, 1)
	go func() {
		n, err := r.Read(buf)
		ch <- readResult{n, err}
	}()
	return ch
}

type readAllResult struct {
	data []byte
	err  error
}

func asyncReadAll(r io.Reader) <-chan readAllResult {
	ch := make(chan readAllResult, 1)
	go func() {
		data, err := io.ReadAll(r)
		ch <- readAllResult{data, err}
	}()
	return ch
}

// expectBlocked asserts an in-flight Read has not returned while the transfer
// is held at a gate. It observes the reader, never the storage size.
func expectBlocked(c *taskcheck.C, ch <-chan readResult, what string) {
	select {
	case res := <-ch:
		c.Check(false, "%s returned early with n=%d err=%v", what, res.n, res.err)
	case <-time.After(100 * time.Millisecond):
		c.Check(true, "%s blocked at the visible prefix as expected", what)
	}
}

func expectRead(c *taskcheck.C, ch <-chan readResult, buf, want []byte) {
	select {
	case res := <-ch:
		c.Check(res.err == nil, "unexpected read error: %v", res.err)
		c.Check(res.n == len(want) && bytes.Equal(buf[:res.n], want),
			"read %d bytes %q, want %q", res.n, buf[:res.n], want)
	case <-time.After(liveWait):
		c.Check(false, "timed out waiting for %q", want)
	}
}

// TestLiveVisibility covers what bytes a stream can see, and when, for disk and
// NoStore storage, including resume and overwrite semantics.
func TestLiveVisibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		payload := []byte("row1\nrow2\nrow3\n") // 15 bytes, five bytes per record

		for _, memory := range []bool{false, true} {
			label := "disk"
			if memory {
				label = "nostore"
			}
			t.Run(label, func(t *testing.T) {
				dir := t.TempDir()
				gate := newLiveGate()
				tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
				req := newLiveRequest(t, filepath.Join(dir, "capture"))
				req.NoStore = memory
				req.RateLimiter = gate

				resp := newLiveClient(tr).Do(req)
				select {
				case <-gate.prefix:
				case <-time.After(liveWait):
					t.Fatal("first chunk was never published")
				}

				// A view opened immediately after Do starts at the beginning and
				// sees exactly the prefix written so far.
				stream, err := resp.OpenStream(context.Background())
				c.Check(err == nil, "OpenStream failed: %v", err)
				defer stream.Close()

				c.Check(!resp.IsComplete(), "transfer still in progress")
				c.Check(resp.BytesComplete() == 5, "visible prefix is 5 bytes, got %d", resp.BytesComplete())

				buf := make([]byte, 5)
				n, err := io.ReadFull(stream, buf)
				c.Check(err == nil, "first record read: %v", err)
				c.Check(n == 5 && string(buf) == "row1\n", "first record = %q", buf[:n])

				// The next Read parks at the end of the visible prefix.
				blockedBuf := make([]byte, 5)
				blocked := asyncRead(stream, blockedBuf)
				expectBlocked(c, blocked, label+" read at prefix end")

				gate.step(t) // admit chunk two
				expectRead(c, blocked, blockedBuf, []byte("row2\n"))

				gate.step(t) // admit chunk three
				gate.step(t) // admit the EOF iteration so the response finalizes
				rest, err := io.ReadAll(stream)
				c.Check(err == nil, "draining remainder: %v", err)
				c.Check(string(rest) == "row3\n", "remainder = %q", rest)

				// EOF only arrives once the whole response has finalized.
				select {
				case <-resp.Done:
					c.Check(true, "response finalized before EOF")
				case <-time.After(liveWait):
					c.Check(false, "response never finalized")
				}
				_, err = stream.Read(make([]byte, 1))
				c.Check(errors.Is(err, io.EOF), "terminal read gives io.EOF, got %v", err)
				c.Check(resp.Err() == nil, "download succeeded: %v", resp.Err())
				c.Check(atomic.LoadInt32(&tr.closes) == 1, "HTTP body closed once, got %d", tr.closes)

				// Legacy completion-only accessors keep their old behavior.
				legacy, err := resp.Open()
				c.Check(err == nil, "legacy Open: %v", err)
				legacyBytes, err := io.ReadAll(legacy)
				c.Check(err == nil && bytes.Equal(legacyBytes, payload), "legacy Open content")
				legacy.Close()
				direct, err := resp.Bytes()
				c.Check(err == nil && bytes.Equal(direct, payload), "legacy Bytes content")
			})
		}

		// A write shorter than the transfer buffer publishes exactly its real
		// length: visibility follows accepted bytes, not buffer size.
		t.Run("short-write-visibility", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: []byte("abcdefghij"), chunk: 3, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "short"))
			req.BufferSize = 32 * 1024
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix

			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "OpenStream: %v", err)
			defer stream.Close()
			got := make([]byte, 3)
			_, err = io.ReadFull(stream, got)
			c.Check(err == nil && string(got) == "abc", "three-byte first write visible, got %q", got)
			c.Check(resp.BytesComplete() == 3, "only 3 bytes complete, got %d", resp.BytesComplete())
			probeBuf := make([]byte, 1)
			blocked := asyncRead(stream, probeBuf)
			expectBlocked(c, blocked, "short-write boundary read")
			gate.step(t) // admit chunk two
			expectRead(c, blocked, probeBuf, []byte("d"))
			gate.step(t) // admit chunk three
			gate.step(t) // admit chunk four
			gate.step(t) // admit the EOF iteration
			rest, err := io.ReadAll(stream)
			c.Check(err == nil && string(rest) == "efghij", "remainder after short write: %q", rest)
			c.Check(resp.Err() == nil, "short-write transfer succeeds: %v", resp.Err())
		})

		// Real resume: the pre-existing local prefix is readable immediately and
		// the ranged suffix is appended behind it.
		t.Run("resume-prefix", func(t *testing.T) {
			dir := t.TempDir()
			dst := filepath.Join(dir, "resume")
			prefix := []byte("row1\nrow") // 8 bytes already on disk
			if err := os.WriteFile(dst, prefix, 0666); err != nil {
				t.Fatal(err)
			}
			payload := []byte("row1\nrow2\nrow3\n") // 15 total, 7 remaining
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, dst)
			req.Size = int64(len(payload))
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)

			select {
			case start := <-tr.rangeSeen:
				c.Check(start == int64(len(prefix)), "GET resumes at %d, got %d", len(prefix), start)
			case <-time.After(liveWait):
				c.Check(false, "no ranged GET observed")
			}

			// The resumed prefix is visible before any suffix byte lands.
			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "OpenStream during resume: %v", err)
			defer stream.Close()
			head := make([]byte, len(prefix))
			_, err = io.ReadFull(stream, head)
			c.Check(err == nil && bytes.Equal(head, prefix), "resumed prefix readable from the start")
			c.Check(resp.DidResume, "Response reports DidResume")

			// Once the first suffix chunk lands it is readable at the boundary.
			<-gate.prefix
			suffixBuf := make([]byte, 5)
			next := asyncRead(stream, suffixBuf)
			expectRead(c, next, suffixBuf, []byte("2\nrow"))

			// The final two bytes then block until released.
			tailBuf := make([]byte, 5)
			blocked := asyncRead(stream, tailBuf)
			expectBlocked(c, blocked, "resume read at the new prefix boundary")
			gate.step(t) // admit the second suffix chunk
			expectRead(c, blocked, tailBuf, []byte("3\n"))
			gate.step(t) // admit the EOF iteration
			all, err := io.ReadAll(stream)
			c.Check(err == nil && len(all) == 0, "resumed view reaches EOF, got %q", all)
			c.Check(resp.Err() == nil, "resumed transfer succeeds: %v", resp.Err())
			onDisk, err := os.ReadFile(dst)
			c.Check(err == nil && bytes.Equal(onDisk, payload), "assembled file matches payload")
		})

		// Overwrite: until BeforeCopy allows the copy and truncation has run, the
		// stale file contents must never be presented as the new response.
		t.Run("overwrite-hides-stale-prefix", func(t *testing.T) {
			dir := t.TempDir()
			dst := filepath.Join(dir, "overwrite")
			stale := []byte("STALE-CONTENT-DO-NOT-READ")
			if err := os.WriteFile(dst, stale, 0666); err != nil {
				t.Fatal(err)
			}
			payload := []byte("fresh-data\n") // 11 bytes: writes of 5, 5, 1
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, dst)
			req.NoResume = true
			req.RateLimiter = gate
			beforeCopy := make(chan struct{})
			req.BeforeCopy = func(*Response) error {
				<-beforeCopy
				return nil
			}
			resp := newLiveClient(tr).Do(req)

			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "OpenStream before truncation: %v", err)
			defer stream.Close()

			probeBuf := make([]byte, len(stale))
			probe := asyncRead(stream, probeBuf)
			expectBlocked(c, probe, "read while overwrite is gated before truncation")
			fi, err := os.Stat(dst)
			c.Check(err == nil && fi.Size() == int64(len(stale)), "stale file untouched until BeforeCopy releases")

			close(beforeCopy) // truncation runs; the first fresh chunk unblocks the probe
			select {
			case res := <-probe:
				c.Check(res.err == nil, "read after truncation: %v", res.err)
				c.Check(res.n == 5 && string(probeBuf[:5]) == "fresh", "first fresh bytes exposed, n=%d", res.n)
				c.Check(!bytes.Contains(probeBuf[:res.n], []byte("STALE")), "no stale bytes exposed")
			case <-time.After(liveWait):
				c.Check(false, "stream never unblocked after truncation")
			}
			<-gate.prefix
			gate.step(t) // admit chunk two
			gate.step(t) // admit chunk three
			gate.step(t) // admit the EOF iteration
			rest, err := io.ReadAll(stream)
			c.Check(err == nil && string(rest) == string(payload[5:]), "fresh remainder: %q", rest)
			c.Check(resp.Err() == nil, "overwrite transfer succeeds: %v", resp.Err())
			onDisk, err := os.ReadFile(dst)
			c.Check(err == nil && bytes.Equal(onDisk, payload), "overwritten file matches payload")
		})
	})
}

// TestLiveTerminal covers success/failure termination: EOF versus the original
// error, post-processing failures, checksum deletion, late opens and nil
// contexts.
func TestLiveTerminal(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		payload := []byte("row1\nrow2\nrow3\n")

		// Mid-transfer failures, for both storage modes: drain the stored prefix,
		// then receive the identifiable download error instead of a fake EOF.
		for _, memory := range []bool{false, true} {
			label := "disk-failure"
			if memory {
				label = "nostore-failure"
			}
			t.Run(label, func(t *testing.T) {
				dir := t.TempDir()
				gate := newLiveGate()
				tr := &liveTransport{payload: payload, chunk: 5, failAt: 10, rangeSeen: make(chan int64, 1)}
				req := newLiveRequest(t, filepath.Join(dir, "fail"))
				req.NoStore = memory
				req.RateLimiter = gate
				resp := newLiveClient(tr).Do(req)
				<-gate.prefix

				stream, err := resp.OpenStream(context.Background())
				c.Check(err == nil, "OpenStream: %v", err)
				defer stream.Close()

				first := make([]byte, 5)
				_, err = io.ReadFull(stream, first)
				c.Check(err == nil && string(first) == "row1\n", "prefix before failure readable")
				gate.step(t) // release write one's wait (write two runs)
				gate.step(t) // release write two's wait (the failing read then runs)
				got, err := io.ReadAll(stream)
				c.Check(bytes.Equal(got, []byte("row2\n")), "stored prefix fully drained, got %q", got)
				c.Check(errors.Is(err, errLiveNet), "original network error surfaced, got %v", err)
				c.Check(!errors.Is(err, io.EOF), "failure is never disguised as EOF")
				c.Check(errors.Is(resp.Err(), errLiveNet), "Response.Err matches stream error")

				// Opening a stream after a known failure returns it directly.
				late, err := resp.OpenStream(context.Background())
				c.Check(late == nil && errors.Is(err, errLiveNet), "late OpenStream returns the terminal error")
			})
		}

		// A Read that returns data and an error together: only the bytes truly
		// written become visible.
		t.Run("data-and-error-read", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: 7, withData: true, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "dataerr"))
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix
			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "OpenStream: %v", err)
			defer stream.Close()
			gate.step(t) // release write one's wait (write two returns data plus error)
			gate.step(t) // release write two's wait so the loop observes the error
			got, err := io.ReadAll(stream)
			c.Check(len(got) == 7, "exactly the 7 accepted bytes are visible, got %d", len(got))
			c.Check(string(got) == "row1\nro", "visible bytes = %q", got)
			c.Check(errors.Is(err, errLiveNet), "terminal error after data+error read: %v", err)
		})

		// Reading the whole network body is not success: an AfterCopy failure
		// holds EOF back and then replaces it with the hook's error.
		t.Run("after-copy-failure", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "hook"))
			req.RateLimiter = gate
			hookGate := make(chan struct{})
			req.AfterCopy = func(*Response) error {
				<-hookGate
				return errLiveHook
			}
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix
			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "OpenStream: %v", err)
			defer stream.Close()

			drain := asyncReadAll(stream)
			gate.step(t) // chunk two
			gate.step(t) // chunk three: the full body is stored
			gate.step(t) // EOF iteration; AfterCopy is now parked on hookGate
			select {
			case <-resp.Done:
				c.Check(false, "response finalized while AfterCopy still running")
			case <-time.After(100 * time.Millisecond):
				c.Check(true, "EOF withheld while AfterCopy is in flight")
			}
			select {
			case res := <-drain:
				c.Check(false, "stream terminated before AfterCopy finished: %q / %v", res.data, res.err)
			case <-time.After(100 * time.Millisecond):
				c.Check(true, "reader stays blocked on the terminal decision")
			}
			close(hookGate)
			select {
			case res := <-drain:
				c.Check(bytes.Equal(res.data, payload), "all stored bytes were handed over: %q", res.data)
				c.Check(errors.Is(res.err, errLiveHook), "hook error reaches the stream, got %v", res.err)
				c.Check(!errors.Is(res.err, io.EOF), "hook failure is not reported as EOF")
			case <-time.After(liveWait):
				c.Check(false, "hook failure never reached the stream")
			}
			c.Check(errors.Is(resp.Err(), errLiveHook), "Response.Err carries the hook error")
		})

		// Checksum mismatch with deleteOnError: an open view drains the bytes it
		// could already see and then receives ErrBadChecksum even though the
		// destination file has been removed.
		t.Run("checksum-delete-keeps-view-readable", func(t *testing.T) {
			dir := t.TempDir()
			dst := filepath.Join(dir, "checksum")
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, dst)
			req.RateLimiter = gate
			req.SetChecksum(md5.New(), bytes.Repeat([]byte{0}, md5.Size), true)
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix
			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "OpenStream: %v", err)
			defer stream.Close()

			first := make([]byte, 5)
			_, err = io.ReadFull(stream, first)
			c.Check(err == nil && string(first) == "row1\n", "prefix readable before validation")
			gate.step(t)
			gate.step(t)
			gate.step(t)
			got, err := io.ReadAll(stream)
			c.Check(bytes.Equal(got, payload[5:]), "visible prefix drained after delete, got %q", got)
			c.Check(errors.Is(err, ErrBadChecksum), "checksum error reaches stream, got %v", err)
			c.Check(resp.Err() == ErrBadChecksum, "Response.Err is ErrBadChecksum")
			_, statErr := os.Stat(dst)
			c.Check(os.IsNotExist(statErr), "deleteOnError removed the destination")
		})

		// When the HTTP request itself fails, no storage is ever created and the
		// transfer is terminal by the time Do returns. Opening a view then hands
		// back the original error instead of a handle that cannot read anything.
		t.Run("early-failure-before-storage", func(t *testing.T) {
			cl := NewClient()
			cl.HTTPClient = errorTransport{err: errLiveNet}
			req := newLiveRequest(t, filepath.Join(t.TempDir(), "early"))
			resp := cl.Do(req)

			c.Check(errors.Is(resp.Err(), errLiveNet), "Response.Err carries the early failure")
			stream, err := resp.OpenStream(context.Background())
			c.Check(stream == nil && errors.Is(err, errLiveNet),
				"OpenStream returns the early failure directly, got stream=%v err=%v", stream, err)
			_, statErr := os.Stat(resp.Filename)
			c.Check(os.IsNotExist(statErr), "no destination file was created")
		})

		// After a successful finish, new views still open and read the whole
		// result; nil contexts are rejected up front.
		t.Run("post-success-open-and-nil-context", func(t *testing.T) {
			dir := t.TempDir()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "done"))
			resp := newLiveClient(tr).Do(req)
			c.Check(resp.Err() == nil, "plain transfer succeeds")

			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "OpenStream after success: %v", err)
			all, err := io.ReadAll(stream)
			c.Check(err == nil && bytes.Equal(all, payload), "post-success view reads full payload")
			_, err = stream.Read(make([]byte, 1))
			c.Check(errors.Is(err, io.EOF), "post-success view ends in EOF")
			c.Check(stream.Close() == nil, "close after EOF is clean")

			nilStream, err := resp.OpenStream(nil)
			c.Check(nilStream == nil && err != nil, "nil context rejected at creation")
		})
	})
}

// TestLiveIsolation covers independent offsets, per-view cancelation/close and
// a download-level failure with open views.
func TestLiveIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		payload := []byte("row1\nrow2\nrow3\n")

		// Two views over one live response read independently from the start at
		// different paces without sharing an offset.
		t.Run("independent-offsets", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "multi"))
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix

			a, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open view A: %v", err)
			defer a.Close()
			b, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open view B: %v", err)
			defer b.Close()

			aFirst := make([]byte, 5)
			_, err = io.ReadFull(a, aFirst)
			c.Check(err == nil && string(aFirst) == "row1\n", "view A reads first record")
			gate.step(t)
			gate.step(t)
			gate.step(t)
			// B is unaffected and starts at byte zero.
			bWhole := make([]byte, 0, len(payload))
			bBuf := make([]byte, 4)
			for {
				n, err := b.Read(bBuf)
				bWhole = append(bWhole, bBuf[:n]...)
				if err != nil {
					c.Check(errors.Is(err, io.EOF), "view B terminates in EOF: %v", err)
					break
				}
			}
			c.Check(bytes.Equal(bWhole, payload), "view B read the whole payload on its own offset")

			// A resumes where it stopped and also reaches the end.
			aRest, err := io.ReadAll(a)
			c.Check(err == nil && string(aFirst)+string(aRest) == string(payload), "view A resumes independently")
			c.Check(resp.Err() == nil, "transfer still succeeds: %v", resp.Err())
			c.Check(atomic.LoadInt32(&tr.closes) == 1, "single HTTP body for both views")
		})

		// Canceling one view's context wakes only that view; the sibling and the
		// download continue.
		t.Run("per-view-context-cancel", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "cancel"))
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix

			ctx, cancel := context.WithCancel(context.Background())
			victim, err := resp.OpenStream(ctx)
			c.Check(err == nil, "open canceled view: %v", err)
			sibling, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open sibling view: %v", err)
			defer sibling.Close()

			// Catch the view up to the current end so the next Read really
			// blocks waiting for growth.
			victimPrefix := make([]byte, 5)
			_, err = io.ReadFull(victim, victimPrefix)
			c.Check(err == nil && string(victimPrefix) == "row1\n", "canceled view drains its prefix")
			blocked := asyncRead(victim, make([]byte, 5))
			expectBlocked(c, blocked, "canceled view waiting for growth")
			cancel()
			select {
			case res := <-blocked:
				c.Check(errors.Is(res.err, context.Canceled), "canceled read reports context.Canceled, got %v", res.err)
			case <-time.After(liveWait):
				c.Check(false, "cancel never woke the blocked read")
			}
			_, err = victim.Read(make([]byte, 1))
			c.Check(errors.Is(err, context.Canceled), "canceled view stays canceled: %v", err)
			c.Check(victim.Close() == nil, "closing a canceled view is safe")

			gate.step(t)
			gate.step(t)
			gate.step(t)
			all, err := io.ReadAll(sibling)
			c.Check(err == nil && bytes.Equal(all, payload), "sibling view completes after peer cancel")
			c.Check(resp.Err() == nil, "download unaffected by view cancel: %v", resp.Err())
		})

		// Close wakes a concurrent Read with os.ErrClosed, is repeatable, and
		// neither cancels the Response nor disturbs sibling views.
		t.Run("close-isolation", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "close"))
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix

			victim, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open closed view: %v", err)
			sibling, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open sibling view: %v", err)
			defer sibling.Close()

			// Catch the view up to the current end so the next Read really
			// blocks waiting for growth.
			victimPrefix := make([]byte, 5)
			_, err = io.ReadFull(victim, victimPrefix)
			c.Check(err == nil && string(victimPrefix) == "row1\n", "closed view drains its prefix")
			blocked := asyncRead(victim, make([]byte, 5))
			expectBlocked(c, blocked, "closed view waiting for growth")
			c.Check(victim.Close() == nil, "first Close succeeds")
			c.Check(victim.Close() == nil, "repeated Close is safe")
			select {
			case res := <-blocked:
				c.Check(errors.Is(res.err, os.ErrClosed), "concurrent Read reports os.ErrClosed, got %v", res.err)
			case <-time.After(liveWait):
				c.Check(false, "Close never woke the blocked read")
			}
			_, err = victim.Read(make([]byte, 1))
			c.Check(errors.Is(err, os.ErrClosed), "post-Close Read reports os.ErrClosed: %v", err)

			gate.step(t)
			gate.step(t)
			gate.step(t)
			all, err := io.ReadAll(sibling)
			c.Check(err == nil && bytes.Equal(all, payload), "sibling view survives peer Close")
			c.Check(resp.Err() == nil, "Response not canceled by view Close: %v", resp.Err())
		})

		// When the download itself fails with views open, each live view drains
		// its stored prefix and then receives the download error, never EOF.
		t.Run("download-failure-wakes-all-views", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: 10, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "allfail"))
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix

			a, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open view A: %v", err)
			defer a.Close()
			b, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open view B: %v", err)
			defer b.Close()

			first := make([]byte, 5)
			_, err = io.ReadFull(a, first)
			c.Check(err == nil && string(first) == "row1\n", "view A prefix")
			gate.step(t) // release write one's wait (write two runs)
			gate.step(t) // release write two's wait (the failing read then runs)
			aRest, aErr := io.ReadAll(a)
			c.Check(string(aRest) == "row2\n" && errors.Is(aErr, errLiveNet), "view A: prefix then net error")
			bAll, bErr := io.ReadAll(b)
			c.Check(string(bAll) == "row1\nrow2\n" && errors.Is(bErr, errLiveNet), "view B: full prefix then net error")
		})

		// Canceling the Response itself ends the download; surviving views drain
		// what was stored and then surface context.Canceled.
		t.Run("response-cancel", func(t *testing.T) {
			dir := t.TempDir()
			gate := newLiveGate()
			tr := &liveTransport{payload: payload, chunk: 5, failAt: -1, rangeSeen: make(chan int64, 1)}
			req := newLiveRequest(t, filepath.Join(dir, "respcancel"))
			req.RateLimiter = gate
			resp := newLiveClient(tr).Do(req)
			<-gate.prefix

			stream, err := resp.OpenStream(context.Background())
			c.Check(err == nil, "open view: %v", err)
			defer stream.Close()
			first := make([]byte, 5)
			_, err = io.ReadFull(stream, first)
			c.Check(err == nil && string(first) == "row1\n", "prefix read before response cancel")

			cancelErr := resp.Cancel()
			c.Check(errors.Is(cancelErr, context.Canceled), "Response.Cancel returns context.Canceled: %v", cancelErr)
			rest, err := io.ReadAll(stream)
			c.Check(len(rest) == 0, "no unwritten bytes invented: %q", rest)
			c.Check(errors.Is(err, context.Canceled), "view receives download cancelation, got %v", err)
		})
	})
}
