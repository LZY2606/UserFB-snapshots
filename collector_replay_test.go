package rehttp

import (
	"context"
	"errors"
	"io"
	"net/http"
	"reflect"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

// This file exercises request-body replay on retries without any real
// network traffic: a scripted RoundTripper, counting streams and channel
// barriers provide deterministic ordering (no sleeps are used to race the
// implementation).

const collectorPayload = "station=7"

// countReadCloser is a fresh, independent replay stream: every call to a
// GetBody factory returns a new one with its own cursor. It records how
// much has been read and whether (and when) it was closed.
type countReadCloser struct {
	id int
	r  *strings.Reader

	mu        sync.Mutex
	reads     int
	bytesRead int
	closed    int32
	closeOnce sync.Once
	closeCh   chan struct{}
}

func newCountStream(id int, data string) *countReadCloser {
	return &countReadCloser{
		id:      id,
		r:       strings.NewReader(data),
		closeCh: make(chan struct{}),
	}
}

func (c *countReadCloser) Read(p []byte) (int, error) {
	n, err := c.r.Read(p)
	c.mu.Lock()
	c.reads++
	c.bytesRead += n
	c.mu.Unlock()
	return n, err
}

func (c *countReadCloser) Close() error {
	c.closeOnce.Do(func() {
		atomic.StoreInt32(&c.closed, 1)
		close(c.closeCh)
	})
	return nil
}

func (c *countReadCloser) BytesRead() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.bytesRead
}

func (c *countReadCloser) IsClosed() bool {
	return atomic.LoadInt32(&c.closed) == 1
}

// bodyFactory is a controllable replay factory standing in for
// Request.GetBody.
type bodyFactory struct {
	mu      sync.Mutex
	calls   int
	streams []*countReadCloser
	data    string

	// failErr makes the factory return this error instead of a stream.
	failErr error
	// nilStream makes the factory return (nil, nil).
	nilStream bool
}

func (f *bodyFactory) getBody() (io.ReadCloser, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls++
	if f.failErr != nil {
		return nil, f.failErr
	}
	if f.nilStream {
		return nil, nil
	}
	s := newCountStream(len(f.streams), f.data)
	f.streams = append(f.streams, s)
	return s, nil
}

func (f *bodyFactory) Calls() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

// trackedRespBody signals when a response body handed back by the
// underlying RoundTripper has been closed.
type trackedRespBody struct {
	*strings.Reader
	closeOnce sync.Once
	closeCh   chan struct{}
}

func newTrackedRespBody(s string) *trackedRespBody {
	return &trackedRespBody{Reader: strings.NewReader(s), closeCh: make(chan struct{})}
}

func (b *trackedRespBody) Close() error {
	b.closeOnce.Do(func() { close(b.closeCh) })
	return nil
}

func (b *trackedRespBody) isClosed() bool {
	select {
	case <-b.closeCh:
		return true
	default:
		return false
	}
}

func newResponse(status int, body string) *http.Response {
	return &http.Response{
		StatusCode: status,
		Body:       newTrackedRespBody(body),
		Header:     make(http.Header),
	}
}

// scriptedRoundTripper records every request attempt and dispatches to a
// per-attempt script function.
type scriptedRoundTripper struct {
	mu   sync.Mutex
	seen []*http.Request
	fn   func(attempt int, req *http.Request) (*http.Response, error)
}

func (s *scriptedRoundTripper) RoundTrip(req *http.Request) (*http.Response, error) {
	s.mu.Lock()
	idx := len(s.seen)
	s.seen = append(s.seen, req)
	fn := s.fn
	s.mu.Unlock()
	return fn(idx, req)
}

func (s *scriptedRoundTripper) calls() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.seen)
}

func (s *scriptedRoundTripper) request(i int) *http.Request {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.seen[i]
}

func newCollectorRequest(t *testing.T, body io.ReadCloser, getBody func() (io.ReadCloser, error)) *http.Request {
	t.Helper()
	req, err := http.NewRequest(http.MethodPost, "http://example.com/collect", body)
	if err != nil {
		t.Fatalf("new request: %v", err)
	}
	req.Header.Set("Content-Type", "application/json")
	req.GetBody = getBody
	return req
}

func retryServiceUnavailable(maxAttempts int) RetryFn {
	return RetryAll(
		RetryMaxRetries(maxAttempts),
		func(a Attempt) bool {
			return a.Response != nil && a.Response.StatusCode == http.StatusServiceUnavailable
		},
	)
}

type roundTripResult struct {
	res *http.Response
	err error
}

func TestCollectorBodyReplay(t *testing.T) {
	// 1. The first attempt is lazy: the original Body is handed to the
	// underlying RoundTripper untouched, it is not read ahead, and the
	// GetBody factory is not invoked before or during a successful first
	// attempt.
	t.Run("lazy first attempt uses original body without calling GetBody", func(t *testing.T) {
		fac := &bodyFactory{data: collectorPayload}
		s0 := newCountStream(0, collectorPayload)
		req := newCollectorRequest(t, s0, fac.getBody)
		origBody, origGetBody, origHeader := req.Body, req.GetBody, req.Header

		enter := make(chan struct{})
		release := make(chan struct{})
		rt := &scriptedRoundTripper{fn: func(_ int, r *http.Request) (*http.Response, error) {
			close(enter)
			<-release
			b, err := io.ReadAll(r.Body)
			if err != nil {
				return nil, err
			}
			r.Body.Close()
			if string(b) != collectorPayload {
				t.Errorf("body = %q, want %q", b, collectorPayload)
			}
			return newResponse(http.StatusOK, "ok"), nil
		}}
		tr := NewTransport(rt, retryServiceUnavailable(2), ConstDelay(0))

		done := make(chan roundTripResult, 1)
		go func() {
			res, err := tr.RoundTrip(req)
			done <- roundTripResult{res, err}
		}()

		<-enter
		if got := fac.Calls(); got != 0 {
			t.Fatalf("GetBody calls before first send = %d, want 0", got)
		}
		if got := s0.BytesRead(); got != 0 {
			t.Fatalf("body bytes read before RoundTripper read = %d, want 0", got)
		}
		if req.Body != origBody {
			t.Fatal("original request Body was replaced before the first attempt")
		}
		close(release)

		res := <-done
		if res.err != nil {
			t.Fatalf("RoundTrip error: %v", res.err)
		}
		if res.res.StatusCode != http.StatusOK {
			t.Fatalf("status = %d, want 200", res.res.StatusCode)
		}
		if got := ioReadAllString(t, res.res.Body); got != "ok" {
			t.Fatalf("response body = %q, want ok", got)
		}
		res.res.Body.Close()

		if fac.Calls() != 0 {
			t.Errorf("GetBody was called %d times, want 0 on a single successful attempt", fac.Calls())
		}
		if !s0.IsClosed() || s0.BytesRead() != len(collectorPayload) {
			t.Errorf("first stream: closed=%v bytesRead=%d, want closed and %d bytes",
				s0.IsClosed(), s0.BytesRead(), len(collectorPayload))
		}
		if rt.request(0).Body != io.ReadCloser(s0) {
			t.Error("first attempt did not receive the original body stream")
		}
		if req.Body != origBody {
			t.Error("original request Body was replaced")
		}
		if req.GetBody == nil || reflect.ValueOf(req.GetBody).Pointer() != reflect.ValueOf(origGetBody).Pointer() {
			t.Error("original request GetBody was replaced")
		}
		if reflect.ValueOf(req.Header).Pointer() != reflect.ValueOf(origHeader).Pointer() {
			t.Error("original request Header map was replaced")
		}
	})

	// 2. Interleaved streams: the underlying RoundTripper reads only the
	// first two bytes of the first stream before returning a retryable
	// 503, and may keep draining that stream asynchronously afterwards.
	// The retry must run on a brand new stream from the factory; the two
	// cursors and their close lifecycle stay independent.
	t.Run("retry stream is independent of the stream still in use below", func(t *testing.T) {
		fac := &bodyFactory{data: collectorPayload}
		s0 := newCountStream(0, collectorPayload)
		req := newCollectorRequest(t, s0, fac.getBody)
		req.ContentLength = int64(len(collectorPayload))

		firstRead := make(chan struct{})
		secondEntered := make(chan struct{})
		secondRelease := make(chan struct{})
		drainFirst := make(chan struct{})
		firstRespBody := newTrackedRespBody("busy")

		rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
			switch attempt {
			case 0:
				p := make([]byte, 2)
				if n, err := io.ReadFull(r.Body, p); err != nil || n != 2 || string(p) != "st" {
					t.Fatalf("first attempt read = %q (n=%d, err=%v), want st", p, n, err)
				}
				close(firstRead)

				// The underlying transport may finish draining the
				// surrendered stream only after returning a retryable
				// result.
				go func(body io.ReadCloser) {
					<-drainFirst
					rest, err := io.ReadAll(body)
					if err != nil {
						t.Errorf("async drain of first stream: %v", err)
					}
					if string(rest) != "ation=7" {
						t.Errorf("first stream remainder = %q, want ation=7", rest)
					}
					body.Close()
				}(r.Body)

				return &http.Response{
					StatusCode: http.StatusServiceUnavailable,
					Body:       firstRespBody,
					Header:     make(http.Header),
				}, nil
			default:
				close(secondEntered)
				<-secondRelease
				b, err := io.ReadAll(r.Body)
				if err != nil {
					return nil, err
				}
				if string(b) != collectorPayload {
					t.Errorf("second attempt body = %q, want %q", b, collectorPayload)
				}
				r.Body.Close() // the RoundTripper owns every surrendered request body
				return newResponse(http.StatusOK, "ok"), nil
			}
		}}
		tr := NewTransport(rt, retryServiceUnavailable(1), ConstDelay(0))

		done := make(chan roundTripResult, 1)
		go func() {
			res, err := tr.RoundTrip(req)
			done <- roundTripResult{res, err}
		}()

		<-firstRead
		if got := s0.BytesRead(); got != 2 {
			t.Fatalf("first stream position after 503 = %d, want 2", got)
		}

		<-secondEntered
		// The retry has a fresh stream; the first stream must not have
		// been rewound or touched meanwhile.
		if fac.Calls() != 1 {
			t.Fatalf("GetBody calls = %d, want 1", fac.Calls())
		}
		if got := s0.BytesRead(); got != 2 {
			t.Fatalf("first stream moved to %d bytes, want it to stay at 2", got)
		}
		if len(fac.streams) != 1 || fac.streams[0] == s0 {
			t.Fatal("factory did not produce a distinct second stream")
		}
		s1 := fac.streams[0]
		if s1.BytesRead() != 0 {
			t.Fatal("replay stream was not fresh")
		}
		if !firstRespBody.isClosed() {
			t.Fatal("discarded 503 response body was not released before retrying")
		}
		// Release the second attempt now that both streams' independent
		// cursors have been asserted.
		close(secondRelease)

		// Now let the underlying transport finish with the old stream.
		close(drainFirst)

		res := <-done
		if res.err != nil {
			t.Fatalf("RoundTrip error: %v", res.err)
		}
		if res.res.StatusCode != http.StatusOK {
			t.Fatalf("status = %d, want 200", res.res.StatusCode)
		}
		// A successful response body stays readable (and uncancelled)
		// until the caller is done with it.
		if body := res.res.Body; body == nil {
			t.Fatal("successful response has no body")
		}
		if got := ioReadAllString(t, res.res.Body); got != "ok" {
			t.Fatalf("success body = %q, want ok", got)
		}
		res.res.Body.Close()

		<-s0.closeCh
		if s0.BytesRead() != len(collectorPayload) {
			t.Errorf("first stream read %d bytes in total, want %d", s0.BytesRead(), len(collectorPayload))
		}
		if !s1.IsClosed() || s1.BytesRead() != len(collectorPayload) {
			t.Errorf("second stream: closed=%v bytesRead=%d, want closed and %d bytes",
				s1.IsClosed(), s1.BytesRead(), len(collectorPayload))
		}

		// The retry request clones the immutable request fields instead
		// of mutating the original request.
		r0, r1 := rt.request(0), rt.request(1)
		if r1 == req || r1.Body == s0 {
			t.Fatal("retry reused the original request or its body")
		}
		if r1.Body != io.ReadCloser(s1) {
			t.Error("retry request does not carry the factory stream")
		}
		if r1.ContentLength != r0.ContentLength {
			t.Error("retry lost ContentLength")
		}
		if r1.GetBody == nil || reflect.ValueOf(r1.GetBody).Pointer() != reflect.ValueOf(req.GetBody).Pointer() {
			t.Error("retry lost GetBody")
		}
		if r1.Cancel != req.Cancel {
			t.Error("retry lost Cancel")
		}
		if reflect.ValueOf(r1.Header).Pointer() != reflect.ValueOf(req.Header).Pointer() {
			t.Error("retry did not keep the original Header")
		}
		// Without a per-attempt timeout the clone keeps the original
		// parent context; with one, each attempt wraps it separately
		// (covered in its own subtest). Either way the parent stays valid.
		if r1.Context() != req.Context() {
			t.Error("retry did not keep the original parent context")
		}
		if req.Context().Err() != nil {
			t.Error("original parent context was cancelled")
		}
		if req.Body != io.ReadCloser(s0) {
			t.Error("original request Body was replaced")
		}
	})

	// 3. Factory failures stop the retry with a traceable error; an empty
	// request must never be sent in place of the replay.
	t.Run("GetBody failure aborts the retry", func(t *testing.T) {
		boom := errors.New("factory boom")

		t.Run("factory returns an error", func(t *testing.T) {
			fac := &bodyFactory{data: collectorPayload, failErr: boom}
			s0 := newCountStream(0, collectorPayload)
			req := newCollectorRequest(t, s0, fac.getBody)
			firstRespBody := newTrackedRespBody("busy")

			rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
				if attempt != 0 {
					t.Fatalf("underlying RoundTripper called for attempt %d after factory failure", attempt)
				}
				io.ReadAll(r.Body)
				r.Body.Close()
				return &http.Response{
					StatusCode: http.StatusServiceUnavailable,
					Body:       firstRespBody,
					Header:     make(http.Header),
				}, nil
			}}
			tr := NewTransport(rt, retryServiceUnavailable(1), ConstDelay(0))

			res, err := tr.RoundTrip(req)
			if res != nil {
				t.Errorf("response = %v, want nil after factory failure", res)
			}
			if !errors.Is(err, boom) {
				t.Fatalf("error %v does not wrap factory error %v", err, boom)
			}
			if rt.calls() != 1 || fac.Calls() != 1 {
				t.Fatalf("rt calls = %d, factory calls = %d, want 1 each", rt.calls(), fac.Calls())
			}
			if !firstRespBody.isClosed() {
				t.Error("discarded 503 body was not released before the factory was called")
			}
			if !s0.IsClosed() {
				t.Error("first attempt stream was not closed by the underlying RoundTripper")
			}
		})

		t.Run("factory returns nil stream without error", func(t *testing.T) {
			fac := &bodyFactory{data: collectorPayload, nilStream: true}
			s0 := newCountStream(0, collectorPayload)
			req := newCollectorRequest(t, s0, fac.getBody)

			rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
				io.ReadAll(r.Body)
				r.Body.Close()
				return newResponse(http.StatusServiceUnavailable, "busy"), nil
			}}
			tr := NewTransport(rt, retryServiceUnavailable(1), ConstDelay(0))

			res, err := tr.RoundTrip(req)
			if res != nil {
				t.Errorf("response = %v, want nil", res)
			}
			if !errors.Is(err, errNilGetBody) {
				t.Fatalf("error = %v, want errNilGetBody", err)
			}
			if rt.calls() != 1 || fac.Calls() != 1 {
				t.Fatalf("rt calls = %d, factory calls = %d, want 1 each", rt.calls(), fac.Calls())
			}
		})
	})

	// 4. Cancellation that lands while the retry delay is running stops
	// the retry without ever invoking the factory.
	t.Run("parent cancellation during delay skips the factory", func(t *testing.T) {
		parent, cancel := context.WithCancel(context.Background())
		defer cancel()

		fac := &bodyFactory{data: collectorPayload}
		s0 := newCountStream(0, collectorPayload)
		req := newCollectorRequest(t, s0, fac.getBody).WithContext(parent)
		firstRespBody := newTrackedRespBody("busy")
		attemptReturned := make(chan struct{})

		rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
			if attempt != 0 {
				t.Fatalf("RoundTripper called for attempt %d after cancellation", attempt)
			}
			io.ReadAll(r.Body)
			r.Body.Close()
			close(attemptReturned)
			return &http.Response{
				StatusCode: http.StatusServiceUnavailable,
				Body:       firstRespBody,
				Header:     make(http.Header),
			}, nil
		}}
		// A long delay keeps the transport parked in the delay select;
		// the parent context, not the timer, releases it.
		tr := NewTransport(rt, retryServiceUnavailable(1), ConstDelay(time.Hour))

		done := make(chan roundTripResult, 1)
		go func() {
			res, err := tr.RoundTrip(req)
			done <- roundTripResult{res, err}
		}()

		<-attemptReturned
		<-firstRespBody.closeCh // the disposed 503 body is released before parking
		cancel()

		res := <-done
		if res.res != nil {
			t.Errorf("response = %v, want nil after cancellation", res.res)
		}
		if res.err == nil || !strings.Contains(res.err.Error(), "request canceled") {
			t.Fatalf("error = %v, want request canceled", res.err)
		}
		if fac.Calls() != 0 {
			t.Errorf("GetBody calls = %d, want 0 after cancellation", fac.Calls())
		}
		if rt.calls() != 1 {
			t.Errorf("RoundTripper calls = %d, want 1", rt.calls())
		}
		if parent.Err() != context.Canceled {
			t.Error("parent context did not propagate the cancellation")
		}
	})

	// 5. PerAttemptTimeout builds a fresh, independent per-attempt
	// context: the second attempt must not inherit the first one's
	// cancellation, while the parent context stays valid throughout.
	t.Run("each attempt gets its own timeout context", func(t *testing.T) {
		parent, cancel := context.WithCancel(context.Background())
		defer cancel()

		fac := &bodyFactory{data: collectorPayload}
		req := newCollectorRequest(t, newCountStream(0, collectorPayload), fac.getBody).WithContext(parent)

		type ctxInfo struct {
			ctx      context.Context
			deadline time.Time
		}
		infos := make(chan ctxInfo, 2)

		rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
			dl, ok := r.Context().Deadline()
			if !ok {
				t.Fatal("per-attempt context has no deadline")
			}
			infos <- ctxInfo{r.Context(), dl}

			if attempt == 0 {
				<-r.Context().Done()
				if !errors.Is(r.Context().Err(), context.DeadlineExceeded) {
					t.Errorf("first attempt err = %v, want DeadlineExceeded", r.Context().Err())
				}
				r.Body.Close()
				return nil, r.Context().Err()
			}

			select {
			case <-r.Context().Done():
				t.Fatal("second attempt inherited an already-cancelled context")
			default:
			}
			b, err := io.ReadAll(r.Body)
			if err != nil {
				return nil, err
			}
			if string(b) != collectorPayload {
				t.Errorf("second attempt body = %q, want %q", b, collectorPayload)
			}
			r.Body.Close()
			return newResponse(http.StatusOK, "ok"), nil
		}}
		tr := NewTransport(rt,
			RetryAll(RetryMaxRetries(1), RetryIsErr(func(err error) bool {
				return errors.Is(err, context.DeadlineExceeded)
			})),
			ConstDelay(0))
		tr.PerAttemptTimeout = 100 * time.Millisecond

		res, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip error: %v", err)
		}
		if res.StatusCode != http.StatusOK {
			t.Fatalf("status = %d, want 200", res.StatusCode)
		}
		if got := ioReadAllString(t, res.Body); got != "ok" {
			t.Fatalf("success body = %q, want ok", got)
		}
		res.Body.Close()

		close(infos)
		var got []ctxInfo
		for info := range infos {
			got = append(got, info)
		}
		if len(got) != 2 {
			t.Fatalf("recorded %d attempts, want 2", len(got))
		}
		if got[0].ctx == got[1].ctx {
			t.Fatal("both attempts share one per-attempt context")
		}
		if !got[1].deadline.After(got[0].deadline) {
			t.Errorf("second deadline %v is not after first deadline %v", got[1].deadline, got[0].deadline)
		}
		if !errors.Is(got[0].ctx.Err(), context.DeadlineExceeded) {
			t.Errorf("first per-attempt context err = %v, want DeadlineExceeded", got[0].ctx.Err())
		}
		if parent.Err() != nil {
			t.Errorf("parent context err = %v, want nil", parent.Err())
		}
		if fac.Calls() != 1 || len(fac.streams) != 1 {
			t.Fatalf("factory calls = %d, streams = %d, want 1 call producing 1 fresh stream",
				fac.Calls(), len(fac.streams))
		}
	})

	// 6. Legacy path: a request without GetBody is still buffered in
	// memory and replayed from the buffer.
	t.Run("request without GetBody keeps the buffered retry path", func(t *testing.T) {
		s0 := newCountStream(0, collectorPayload)
		req := newCollectorRequest(t, s0, nil)
		if req.GetBody != nil {
			t.Fatal("test setup: stdlib gave this body a GetBody")
		}

		bodies := make(chan string, 2)
		firstRespBody := newTrackedRespBody("busy")
		rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
			// Buffering happens before the first RoundTripper call.
			if !s0.IsClosed() || s0.BytesRead() != len(collectorPayload) {
				t.Errorf("original stream: closed=%v bytesRead=%d, want fully buffered",
					s0.IsClosed(), s0.BytesRead())
			}
			b, err := io.ReadAll(r.Body)
			if err != nil {
				return nil, err
			}
			r.Body.Close()
			bodies <- string(b)
			if attempt == 0 {
				return &http.Response{
					StatusCode: http.StatusServiceUnavailable,
					Body:       firstRespBody,
					Header:     make(http.Header),
				}, nil
			}
			return newResponse(http.StatusOK, "ok"), nil
		}}
		tr := NewTransport(rt, retryServiceUnavailable(1), ConstDelay(0))

		res, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip error: %v", err)
		}
		if res.StatusCode != http.StatusOK {
			t.Fatalf("status = %d, want 200", res.StatusCode)
		}
		res.Body.Close()

		close(bodies)
		var got []string
		for b := range bodies {
			got = append(got, b)
		}
		if len(got) != 2 || got[0] != collectorPayload || got[1] != collectorPayload {
			t.Fatalf("attempt bodies = %v, want two copies of %q", got, collectorPayload)
		}
		if !firstRespBody.isClosed() {
			t.Error("discarded 503 body was not released")
		}
	})

	// 7. PreventRetryWithBody still means exactly one attempt and the
	// factory must never be touched.
	t.Run("PreventRetryWithBody sends once and never calls the factory", func(t *testing.T) {
		fac := &bodyFactory{data: collectorPayload}
		s0 := newCountStream(0, collectorPayload)
		req := newCollectorRequest(t, s0, fac.getBody)

		rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
			if attempt != 0 {
				t.Fatalf("RoundTripper called for attempt %d despite PreventRetryWithBody", attempt)
			}
			io.ReadAll(r.Body)
			r.Body.Close()
			return newResponse(http.StatusServiceUnavailable, "busy"), nil
		}}
		tr := NewTransport(rt, retryServiceUnavailable(1), ConstDelay(0))
		tr.PreventRetryWithBody = true

		res, err := tr.RoundTrip(req)
		if err != nil {
			t.Fatalf("RoundTrip error: %v", err)
		}
		if res.StatusCode != http.StatusServiceUnavailable {
			t.Fatalf("status = %d, want 503", res.StatusCode)
		}
		res.Body.Close()
		if rt.calls() != 1 {
			t.Errorf("RoundTripper calls = %d, want 1", rt.calls())
		}
		if fac.Calls() != 0 {
			t.Errorf("GetBody calls = %d, want 0", fac.Calls())
		}
		if !s0.IsClosed() {
			t.Error("request body was not closed by the underlying RoundTripper")
		}
	})

	// 8. Nil bodies and http.NoBody still retry normally without a
	// factory or buffering.
	t.Run("nil body and http.NoBody still retry", func(t *testing.T) {
		cases := []struct {
			name string
			body io.ReadCloser
		}{
			{"nil body", nil},
			{"http.NoBody", http.NoBody},
		}
		for _, tc := range cases {
			t.Run(tc.name, func(t *testing.T) {
				req, err := http.NewRequest(http.MethodPost, "http://example.com/collect", nil)
				if err != nil {
					t.Fatal(err)
				}
				if tc.body != nil {
					req.Body = tc.body
				}

				rt := &scriptedRoundTripper{fn: func(attempt int, r *http.Request) (*http.Response, error) {
					if attempt == 0 {
						return newResponse(http.StatusServiceUnavailable, "busy"), nil
					}
					return newResponse(http.StatusOK, "ok"), nil
				}}
				tr := NewTransport(rt, retryServiceUnavailable(1), ConstDelay(0))

				res, err := tr.RoundTrip(req)
				if err != nil {
					t.Fatalf("RoundTrip error: %v", err)
				}
				if res.StatusCode != http.StatusOK {
					t.Fatalf("status = %d, want 200", res.StatusCode)
				}
				if got := ioReadAllString(t, res.Body); got != "ok" {
					t.Fatalf("response body = %q, want ok", got)
				}
				res.Body.Close()
				if rt.calls() != 2 {
					t.Errorf("RoundTripper calls = %d, want 2", rt.calls())
				}
			})
		}
	})
}

func ioReadAllString(t *testing.T, r io.Reader) string {
	t.Helper()
	b, err := io.ReadAll(r)
	if err != nil {
		t.Fatalf("read all: %v", err)
	}
	return string(b)
}
