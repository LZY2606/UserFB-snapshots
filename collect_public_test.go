package mastodon_test

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/mattn/go-mastodon"
	"github.com/mattn/go-mastodon/task_support/taskcheck"
)

// collectRequest is a snapshot of one request seen by collectTransport.
type collectRequest struct {
	path          string
	host          string
	query         url.Values
	authorization string
	userAgent     string
}

// collectTransport is a scriptable RoundTripper: streaming requests receive a
// controllable SSE body, history requests are answered by a scripted function.
type collectTransport struct {
	mu       sync.Mutex
	records  []collectRequest
	streamN  int
	historyN int
	handler  func(*http.Request, int) (*http.Response, error)
}

func (t *collectTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	t.mu.Lock()
	t.records = append(t.records, collectRequest{
		path:          req.URL.Path,
		host:          req.URL.Host,
		query:         req.URL.Query(),
		authorization: req.Header.Get("Authorization"),
		userAgent:     req.UserAgent(),
	})
	n := 0
	if req.URL.Path == "/api/v1/streaming/public" || req.URL.Path == "/api/v1/streaming/public/local" {
		t.streamN++
		n = t.streamN
	} else {
		t.historyN++
		n = t.historyN
	}
	handler := t.handler
	t.mu.Unlock()

	return handler(req, n)
}

func (t *collectTransport) snapshot() ([]collectRequest, int, int) {
	t.mu.Lock()
	defer t.mu.Unlock()
	return append([]collectRequest(nil), t.records...), t.streamN, t.historyN
}

// collectFeed is a controllable SSE body.  Reads block until data is queued
// or the feed is closed.
type collectFeed struct {
	mu     sync.Mutex
	cond   *sync.Cond
	chunks [][]byte
	closed bool
}

func newCollectFeed() *collectFeed {
	f := &collectFeed{}
	f.cond = sync.NewCond(&f.mu)
	return f
}

func (f *collectFeed) WriteString(s string) {
	f.mu.Lock()
	f.chunks = append(f.chunks, []byte(s))
	f.cond.Broadcast()
	f.mu.Unlock()
}

func (f *collectFeed) WriteEvent(name, data string) {
	f.WriteString("event: " + name + "\ndata: " + data + "\n\n")
}

func (f *collectFeed) Close() error {
	f.mu.Lock()
	f.closed = true
	f.cond.Broadcast()
	f.mu.Unlock()
	return nil
}

func (f *collectFeed) Read(p []byte) (int, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	for len(f.chunks) == 0 && !f.closed {
		f.cond.Wait()
	}
	if len(f.chunks) == 0 {
		return 0, io.EOF
	}
	chunk := f.chunks[0]
	n := copy(p, chunk)
	if n == len(chunk) {
		f.chunks = f.chunks[1:]
	} else {
		f.chunks[0] = chunk[n:]
	}
	return n, nil
}

// pending reports how much written data the reader has not consumed yet.
func (f *collectFeed) pending() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	n := 0
	for _, chunk := range f.chunks {
		n += len(chunk)
	}
	return n
}

// collectStreamBody records whether this collection's response was closed.
type collectStreamBody struct {
	*collectFeed
	mu     sync.Mutex
	closed bool
}

func (b *collectStreamBody) Close() error {
	b.mu.Lock()
	b.closed = true
	b.mu.Unlock()
	return b.collectFeed.Close()
}

func (b *collectStreamBody) isClosed() bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.closed
}

type collectFixture struct {
	t          *testing.T
	client     *mastodon.Client
	transport  *collectTransport
	feed       *collectFeed
	body       *collectStreamBody
	historyFn  func(*http.Request, int) (*http.Response, error)
	streamCode int
}

func newCollectFixture(t *testing.T) *collectFixture {
	t.Helper()
	feed := newCollectFeed()
	fx := &collectFixture{t: t, feed: feed, transport: &collectTransport{}}
	fx.body = &collectStreamBody{collectFeed: feed}
	fx.streamCode = http.StatusOK
	fx.transport.handler = fx.handle
	fx.client = mastodon.NewClient(&mastodon.Config{
		Server:      "https://collect.invalid",
		AccessToken: "collect-token",
	})
	fx.client.UserAgent = "collect-agent"
	fx.client.Transport = fx.transport
	return fx
}

func (fx *collectFixture) handle(req *http.Request, n int) (*http.Response, error) {
	isStream := req.URL.Path == "/api/v1/streaming/public" || req.URL.Path == "/api/v1/streaming/public/local"
	if isStream {
		if fx.streamCode != http.StatusOK {
			return jsonResponse(req, fx.streamCode, `{"error":"stream unavailable"}`, ""), nil
		}
		h := make(http.Header)
		h.Set("Content-Type", "text/event-stream")
		return &http.Response{StatusCode: http.StatusOK, Header: h, Body: fx.body, Request: req}, nil
	}
	if fx.historyFn == nil {
		return jsonResponse(req, http.StatusOK, `[]`, ""), nil
	}
	return fx.historyFn(req, n)
}

func (fx *collectFixture) start(ctx context.Context, isLocal bool, since mastodon.ID, capacity int, emit func(mastodon.Event) error) <-chan error {
	done := make(chan error, 1)
	go func() {
		done <- fx.client.CollectPublic(ctx, isLocal, since, capacity, emit)
	}()
	return done
}

func jsonResponse(req *http.Request, status int, body, link string) *http.Response {
	h := make(http.Header)
	h.Set("Content-Type", "application/json")
	if link != "" {
		h.Set("Link", link)
	}
	return &http.Response{
		StatusCode: status,
		Header:     h,
		Body:       io.NopCloser(strings.NewReader(body)),
		Request:    req,
	}
}

// collectRecorder records emitted events.  It can block one selected event
// inside emit and report when that event has started.
type collectRecorder struct {
	mu            sync.Mutex
	events        []mastodon.Event
	err           error
	gateOn        bool
	gateID        string
	gate          chan struct{}
	entered       chan struct{}
	enteredClosed bool
}

func (r *collectRecorder) emit(ev mastodon.Event) error {
	r.mu.Lock()
	id, _ := eventID(ev)
	block := r.gateOn && r.gate != nil && r.gateID != "" && id == r.gateID
	gate := r.gate
	entered := r.entered
	if block && entered != nil && !r.enteredClosed {
		r.enteredClosed = true
		close(entered)
	}
	r.mu.Unlock()

	if block {
		<-gate
	}

	r.mu.Lock()
	defer r.mu.Unlock()
	if r.err != nil {
		return r.err
	}
	r.events = append(r.events, ev)
	return nil
}

func (r *collectRecorder) snapshot() []mastodon.Event {
	r.mu.Lock()
	defer r.mu.Unlock()
	return append([]mastodon.Event(nil), r.events...)
}

func (r *collectRecorder) count() int {
	r.mu.Lock()
	defer r.mu.Unlock()
	return len(r.events)
}

func (r *collectRecorder) setError(err error) {
	r.mu.Lock()
	r.err = err
	r.mu.Unlock()
}

func (r *collectRecorder) blockNext(id string) (<-chan struct{}, func()) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.gateOn = true
	r.gateID = id
	r.gate = make(chan struct{})
	r.entered = make(chan struct{})
	gate := r.gate
	return r.entered, func() { close(gate) }
}

func eventID(ev mastodon.Event) (string, bool) {
	switch e := ev.(type) {
	case *mastodon.UpdateEvent:
		return string(e.Status.ID), true
	case *mastodon.UpdateEditEvent:
		return string(e.Status.ID), true
	case *mastodon.DeleteEvent:
		return string(e.ID), true
	}
	return "", false
}

func eventIDs(events []mastodon.Event) []string {
	ids := make([]string, 0, len(events))
	for _, ev := range events {
		id, _ := eventID(ev)
		ids = append(ids, id)
	}
	return ids
}

func updateContent(ev mastodon.Event) string {
	switch e := ev.(type) {
	case *mastodon.UpdateEvent:
		return e.Status.Content
	case *mastodon.UpdateEditEvent:
		return e.Status.Content
	}
	return ""
}

var errCollectTimeout = errors.New("collect did not return in time")

func waitCollect(t *testing.T, done <-chan error, d time.Duration) error {
	t.Helper()
	select {
	case err := <-done:
		return err
	case <-time.After(d):
		return errCollectTimeout
	}
}

func waitCount(t *testing.T, r *collectRecorder, n int, d time.Duration) bool {
	t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if r.count() >= n {
			return true
		}
		time.Sleep(2 * time.Millisecond)
	}
	return r.count() >= n
}

func waitChan(t *testing.T, ch <-chan struct{}, d time.Duration) bool {
	t.Helper()
	select {
	case <-ch:
		return true
	case <-time.After(d):
		return false
	}
}

// waitPending waits until the feed's buffer is fully consumed by the reader.
func waitPending(t *testing.T, f *collectFeed, want int, d time.Duration) bool {
	t.Helper()
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if f.pending() == want {
			return true
		}
		time.Sleep(time.Millisecond)
	}
	return f.pending() == want
}

func TestCollectBootstrap(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Reject invalid arguments before touching the network.
		{
			fx := newCollectFixture(t)
			err := fx.client.CollectPublic(context.Background(), false, mastodon.ID("12x"), 1, func(mastodon.Event) error { return nil })
			c.Check(err != nil, "invalid since ID must be rejected, got %v", err)
			err = fx.client.CollectPublic(context.Background(), false, "", 0, func(mastodon.Event) error { return nil })
			c.Check(err != nil, "capacity below 1 must be rejected, got %v", err)
			err = fx.client.CollectPublic(context.Background(), false, "", 1, nil)
			c.Check(err != nil, "nil emit must be rejected, got %v", err)
			records, streams, history := fx.transport.snapshot()
			c.Check(len(records) == 0 && streams == 0 && history == 0,
				"validation errors must not start network work: records=%d streams=%d history=%d", len(records), streams, history)
		}

		// Full bootstrap: stream first, then paged history, ascending output.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				switch n {
				case 1:
					return jsonResponse(req, http.StatusOK,
						`[{"id":"103"},{"id":"102"}]`,
						`<https://foreign.invalid/unused?max_id=102>; rel="next"`), nil
				case 2:
					return jsonResponse(req, http.StatusOK,
						`[{"id":"104"},{"id":"102"},{"id":"0101"},{"id":"100"},{"id":"099"}]`,
						""), nil
				default:
					return jsonResponse(req, http.StatusOK, `[]`, ""), nil
				}
			}
			ctx, cancel := context.WithCancel(context.Background())
			rec := &collectRecorder{}
			done := fx.start(ctx, true, "100", 4, rec.emit)
			c.Check(waitCount(t, rec, 3, 2*time.Second), "history statuses should be emitted")
			cancel()
			err := waitCollect(t, done, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "cancellation must be reported, got %v", err)

			events := rec.snapshot()
			c.Check(len(events) == 3, "history should contain 3 statuses, got %d", len(events))
			ids := eventIDs(events)
			c.Check(fmt.Sprint(ids) == "[0101 102 103]", "history must be ascending and preserve IDs, got %v", ids)
			for _, ev := range events {
				_, ok := ev.(*mastodon.UpdateEvent)
				c.Check(ok, "history events must be UpdateEvent, got %T", ev)
			}

			records, streams, history := fx.transport.snapshot()
			c.Check(streams == 1, "exactly one streaming request, got %d", streams)
			c.Check(history == 2, "history should stop when the Link disappears, got %d requests", history)
			c.Check(len(records) >= 3, "missing request records")
			if len(records) >= 3 {
				c.Check(records[0].path == "/api/v1/streaming/public/local",
					"local stream path first, got %s", records[0].path)
				c.Check(records[1].path == "/api/v1/timelines/public" && records[2].path == "/api/v1/timelines/public",
					"history path after the stream, got %s %s", records[1].path, records[2].path)
				c.Check(records[1].query.Get("since_id") == "100" && records[1].query.Get("local") == "t",
					"first history page must carry since_id and local: %v", records[1].query)
				c.Check(records[1].query.Get("max_id") == "", "first history page must not carry max_id: %v", records[1].query)
				c.Check(records[2].query.Get("since_id") == "100" && records[2].query.Get("local") == "t" && records[2].query.Get("max_id") == "102",
					"later history page must keep since_id/local and use the Link max_id: %v", records[2].query)
				for i, rec := range records {
					c.Check(rec.host == "collect.invalid", "request %d must use the configured host, got %s", i, rec.host)
					c.Check(rec.authorization == "Bearer collect-token", "request %d must use auth, got %q", i, rec.authorization)
					c.Check(rec.userAgent == "collect-agent", "request %d must use UserAgent, got %q", i, rec.userAgent)
					c.Check(strings.HasPrefix(rec.path, "/api/v1/streaming/") || rec.path == "/api/v1/timelines/public",
						"request %d must not query instance info: %s", i, rec.path)
				}
			}
			c.Check(fx.body.isClosed(), "stream response must be closed when the collection ends")
		}

		// History edge cases: invalid status ID, invalid Link cursor,
		// non-decreasing cursor, and an empty page ending history.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK, `[{"id":"not-a-number"}]`, ""), nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(err != nil, "invalid history status ID must fail")
			c.Check(len(rec.snapshot()) == 0, "no history may be delivered on failure")
		}
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK, `[{"id":"103"}]`, `<https://x.invalid/next?max_id=oops>; rel="next"`), nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(err != nil, "invalid pagination max_id must fail")
			c.Check(len(rec.snapshot()) == 0, "no history may be delivered on failure")
		}
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				if n == 1 {
					return jsonResponse(req, http.StatusOK, `[{"id":"103"}]`, `<https://x.invalid/n?max_id=103>; rel="next"`), nil
				}
				return jsonResponse(req, http.StatusOK, `[{"id":"102"}]`, `<https://x.invalid/n?max_id=103>; rel="next"`), nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(err != nil, "non-decreasing max_id must fail")
			c.Check(len(rec.snapshot()) == 0, "no history may be delivered on failure")
		}
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				if n == 1 {
					return jsonResponse(req, http.StatusOK, `[{"id":"103"}]`, `<https://x.invalid/n?max_id=102>; rel="next"`), nil
				}
				return jsonResponse(req, http.StatusOK, `[]`, ""), nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			rec := &collectRecorder{}
			done := fx.start(ctx, false, "", 2, rec.emit)
			c.Check(waitCount(t, rec, 1, 2*time.Second), "empty page must end history and deliver what was collected")
			cancel()
			err := waitCollect(t, done, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "got %v", err)
			ids := eventIDs(rec.snapshot())
			c.Check(fmt.Sprint(ids) == "[103]", "got %v", ids)
			_, _, history := fx.transport.snapshot()
			c.Check(history == 2, "empty page must prevent one more request, got %d", history)
		}

		// IDs wider than uint64 are compared numerically and preserved
		// verbatim in the output.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK,
					`[{"id":"184467440737095516160"},{"id":"184467440737095516151"},{"id":"184467440737095516159"}]`, ""), nil
			}
			fx.feed.WriteEvent("update", `{"id":"184467440737095516161","content":"big-live"}`)
			fx.feed.WriteEvent("update", `{"id":"184467440737095516150","content":"below-since"}`)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			done := fx.start(ctx, false, "184467440737095516150", 4, rec.emit)
			c.Check(waitCount(t, rec, 4, 2*time.Second), "large-ID history and live events should be delivered")
			fx.feed.Close()
			err := waitCollect(t, done, 2*time.Second)
			c.Check(errors.Is(err, io.EOF), "got %v", err)
			ids := eventIDs(rec.snapshot())
			c.Check(fmt.Sprint(ids) == "[184467440737095516151 184467440737095516159 184467440737095516160 184467440737095516161]",
				"large IDs must sort numerically and keep their string form: %v", ids)
			records, streams, history := fx.transport.snapshot()
			c.Check(streams == 1 && history == 1, "one stream and one history request, got %d/%d", streams, history)
			if len(records) >= 2 {
				c.Check(records[0].path == "/api/v1/streaming/public",
					"non-local stream path expected, got %s", records[0].path)
				c.Check(records[1].path == "/api/v1/timelines/public", "non-local history path expected, got %s", records[1].path)
				c.Check(records[1].query.Get("local") == "", "non-local history must not send local: %v", records[1].query)
			}
		}

		// A non-200 stream stops the collection before any history request.
		{
			fx := newCollectFixture(t)
			fx.streamCode = http.StatusInternalServerError
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(err != nil, "non-200 stream must fail")
			records, streams, history := fx.transport.snapshot()
			c.Check(streams == 1, "one stream attempt, got %d", streams)
			c.Check(history == 0, "history must not start before the stream is confirmed, got %d", history)
			c.Check(len(records) == 1, "only the stream request may have been made, got %d", len(records))
		}
	})
}

func TestCollectHandoff(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Stream events that arrive while history is being downloaded are
		// buffered, then delivered in arrival order after the history.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK, `[{"id":"103"},{"id":"102"}]`, ""), nil
			}
			fx.feed.WriteEvent("update", `{"id":"105","content":"live"}`)
			fx.feed.WriteEvent("status.update", `{"id":"103","content":"edited"}`)
			fx.feed.WriteEvent("delete", `101`)
			fx.feed.WriteEvent("update", `{"id":"103","content":"duplicate-of-history"}`)
			fx.feed.WriteEvent("update", `{"id":"104","content":"first-104"}`)
			fx.feed.WriteEvent("update", `{"id":"104","content":"second-104"}`)
			fx.feed.WriteEvent("update", `{"id":"100","content":"at-since"}`)
			fx.feed.WriteEvent("update", `{"id":"099","content":"before-since"}`)
			fx.feed.WriteEvent("status.update", `{"id":"050","content":"old-edit"}`)
			fx.feed.WriteEvent("delete", `020`)

			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			done := fx.start(ctx, true, "100", 8, rec.emit)
			c.Check(waitCount(t, rec, 8, 2*time.Second), "history plus buffered events should be delivered")
			fx.feed.Close()
			err := waitCollect(t, done, 2*time.Second)
			c.Check(errors.Is(err, io.EOF), "normal stream EOF must return io.EOF, got %v", err)

			events := rec.snapshot()
			ids := eventIDs(events)
			c.Check(fmt.Sprint(ids) == "[102 103 105 103 101 104 050 020]",
				"handoff order wrong: %v", ids)
			if len(events) == 8 {
				_, ok := events[0].(*mastodon.UpdateEvent)
				c.Check(ok, "history statuses precede live events, got %T", events[0])
				_, ok = events[2].(*mastodon.UpdateEvent)
				c.Check(ok, "buffered update 105 expected, got %T", events[2])
				edit, ok := events[3].(*mastodon.UpdateEditEvent)
				c.Check(ok && edit.Status.Content == "edited", "history duplication must not swallow the edit: %T %v", events[3], updateContent(events[3]))
				del, ok := events[4].(*mastodon.DeleteEvent)
				c.Check(ok && del.ID == "101", "delete expected, got %T", events[4])
				c.Check(updateContent(events[5]) == "first-104", "first 104 must be kept, got %q", updateContent(events[5]))
				edit, ok = events[6].(*mastodon.UpdateEditEvent)
				c.Check(ok && updateContent(events[6]) == "old-edit", "edits below since must still be delivered: %T", events[6])
				del, ok = events[7].(*mastodon.DeleteEvent)
				c.Check(ok && del.ID == "020", "deletes below since must still be delivered: %T", events[7])
			}
			records, streams, history := fx.transport.snapshot()
			c.Check(streams == 1 && history == 1, "one stream and one history request, got %d/%d", streams, history)
			c.Check(len(records) == 2, "no extra requests expected, got %d", len(records))

			// Since 100 filtered updates but not edits/deletes.
			c.Check(!strings.Contains(fmt.Sprint(ids), "100"), "update at since must be filtered")
		}

		// A malformed stream event stops the collection before any history is
		// delivered.  The history request observes the cancelled stream.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				<-req.Context().Done()
				return nil, req.Context().Err()
			}
			fx.feed.WriteString("event: update\ndata: {\"id\":\n\n")
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(err != nil, "malformed stream JSON must fail")
			c.Check(len(rec.snapshot()) == 0, "no history may be delivered when the stream fails during preparation")
		}

		// An invalid delete ID is a stream decode error too.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				<-req.Context().Done()
				return nil, req.Context().Err()
			}
			fx.feed.WriteEvent("delete", `not-a-number`)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(err != nil, "invalid delete ID must fail")
			c.Check(len(rec.snapshot()) == 0, "no history may be delivered when the stream fails during preparation")
		}

		// Unrelated event types and comments are ignored.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK, `[{"id":"101"}]`, ""), nil
			}
			fx.feed.WriteString(": comment\n\n")
			fx.feed.WriteEvent("notification", `{"id":"1"}`)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			done := fx.start(ctx, false, "", 2, rec.emit)
			c.Check(waitCount(t, rec, 1, 2*time.Second), "history should be delivered")
			fx.feed.Close()
			err := waitCollect(t, done, 2*time.Second)
			c.Check(errors.Is(err, io.EOF), "got %v", err)
			c.Check(len(rec.snapshot()) == 1, "unrelated events must be ignored, got %d", len(rec.snapshot()))
		}
	})
}

func TestCollectLifetime(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Overflow while the history is still being prepared: nothing is
		// delivered and errors.Is finds ErrCollectOverflow.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				<-req.Context().Done()
				return nil, req.Context().Err()
			}
			fx.feed.WriteEvent("update", `{"id":"201"}`)
			fx.feed.WriteEvent("update", `{"id":"202"}`)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 1, rec.emit), 2*time.Second)
			c.Check(errors.Is(err, mastodon.ErrCollectOverflow), "capacity overflow must be reported, got %v", err)
			c.Check(len(rec.snapshot()) == 0, "history must not be delivered after a preparation failure, got %d", len(rec.snapshot()))
			c.Check(fx.body.isClosed(), "stream response must be closed on overflow")
		}

		// Overflow after handoff keeps the already delivered prefix.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK, `[{"id":"100"}]`, ""), nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			done := fx.start(ctx, false, "", 1, rec.emit)
			c.Check(waitCount(t, rec, 1, 2*time.Second), "history should be delivered before the live phase")

			entered, release := rec.blockNext("200")
			fx.feed.WriteEvent("update", `{"id":"200"}`)
			c.Check(waitChan(t, entered, 2*time.Second), "live emit should start")
			// Fill the one-slot queue while 200 is still being emitted, then
			// let the reader observe one more event so it attempts an
			// enqueue that cannot fit.
			fx.feed.WriteEvent("update", `{"id":"201"}`)
			c.Check(waitPending(t, fx.feed, 0, 2*time.Second), "the reader must consume 201 into the queue")
			fx.feed.WriteEvent("update", `{"id":"202"}`)
			c.Check(waitPending(t, fx.feed, 0, 2*time.Second), "the reader must consume 202 and observe overflow")
			release()

			err := waitCollect(t, done, 2*time.Second)
			c.Check(errors.Is(err, mastodon.ErrCollectOverflow), "post-handoff overflow must be reported, got %v", err)
			ids := eventIDs(rec.snapshot())
			c.Check(fmt.Sprint(ids) == "[100 200]", "delivered prefix must survive overflow, got %v", ids)
		}

		// Cancellation closes the response, stops the reader and starts no
		// further callbacks.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK, `[{"id":"101"}]`, ""), nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			rec := &collectRecorder{}
			done := fx.start(ctx, false, "", 4, rec.emit)
			c.Check(waitCount(t, rec, 1, 2*time.Second), "history should be delivered")
			cancel()
			err := waitCollect(t, done, 2*time.Second)
			c.Check(errors.Is(err, context.Canceled), "cancellation must be reported, got %v", err)
			c.Check(fx.body.isClosed(), "stream response must be closed after cancellation")
			time.Sleep(20 * time.Millisecond)
			c.Check(rec.count() == 1, "no callbacks may start after cancellation, got %d", rec.count())
		}

		// A normal EOF during preparation is a failure and delivers no history.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				<-req.Context().Done()
				return nil, req.Context().Err()
			}
			fx.feed.Close()
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(errors.Is(err, io.EOF), "EOF during preparation must return io.EOF, got %v", err)
			c.Check(len(rec.snapshot()) == 0, "no history may be delivered on EOF during preparation")
		}

		// A pagination failure in a later page aborts before any history is
		// delivered.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				if n == 1 {
					return jsonResponse(req, http.StatusOK, `[{"id":"103"}]`, `<https://x.invalid/n?max_id=102>; rel="next"`), nil
				}
				return jsonResponse(req, http.StatusInternalServerError, `{"error":"boom"}`, ""), nil
			}
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			err := waitCollect(t, fx.start(ctx, false, "", 2, rec.emit), 2*time.Second)
			c.Check(err != nil, "history pagination failure must fail")
			c.Check(len(rec.snapshot()) == 0, "no history may be delivered after a pagination failure, got %d", len(rec.snapshot()))
			_, _, history := fx.transport.snapshot()
			c.Check(history == 2, "two history requests expected, got %d", history)
		}

		// Emit errors are returned unchanged.
		{
			fx := newCollectFixture(t)
			fx.historyFn = func(req *http.Request, n int) (*http.Response, error) {
				return jsonResponse(req, http.StatusOK, `[{"id":"101"},{"id":"102"}]`, ""), nil
			}
			sentinel := errors.New("emit failed")
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			rec := &collectRecorder{}
			rec.setError(sentinel)
			err := waitCollect(t, fx.start(ctx, false, "", 4, rec.emit), 2*time.Second)
			c.Check(errors.Is(err, sentinel), "emit error must be returned unchanged, got %v", err)
			c.Check(fx.body.isClosed(), "stream response must be closed on emit error")
		}
	})
}
