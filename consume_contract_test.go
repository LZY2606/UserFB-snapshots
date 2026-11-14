package sse_test

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/tmaxmax/go-sse"
	"github.com/tmaxmax/go-sse/task_support/taskcheck"
)

// ccHeader is a snapshot of the Last-Event-ID header of an attempt.
type ccHeader struct {
	value string
	// present tells whether the header was set at all. An acknowledged empty ID
	// must remove the header, which is different from setting it to "".
	present bool
}

// ccTransport scripts the responses of a Connection and records the headers of
// every attempt.
type ccTransport struct {
	mu       sync.Mutex
	calls    int
	headers  []ccHeader
	attempts func(call int, r *http.Request) (*http.Response, error)
}

func (t *ccTransport) RoundTrip(r *http.Request) (*http.Response, error) {
	t.mu.Lock()
	call := t.calls
	t.calls++
	t.headers = append(t.headers, ccHeader{
		value:   r.Header.Get("Last-Event-ID"),
		present: len(r.Header.Values("Last-Event-ID")) > 0,
	})
	t.mu.Unlock()

	return t.attempts(call, r)
}

func (t *ccTransport) state() (calls int, headers []ccHeader) {
	t.mu.Lock()
	defer t.mu.Unlock()
	return t.calls, append([]ccHeader(nil), t.headers...)
}

// ccBody counts how many times the response body was closed.
type ccBody struct {
	reader io.ReadCloser
	mu     sync.Mutex
	closes int
}

func (b *ccBody) Read(p []byte) (int, error) {
	return b.reader.Read(p)
}

func (b *ccBody) Close() error {
	b.mu.Lock()
	b.closes++
	b.mu.Unlock()
	return b.reader.Close()
}

func (b *ccBody) closeCount() int {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.closes
}

func ccNewBody(r io.ReadCloser) *ccBody {
	return &ccBody{reader: r}
}

func ccResponse(r *http.Request, body *ccBody) *http.Response {
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     http.Header{"Content-Type": {"text/event-stream"}},
		Body:       body,
		Request:    r,
	}
}

func ccRequest(t *testing.T, ctx context.Context, lastEventID string) *http.Request {
	t.Helper()

	r, err := http.NewRequestWithContext(ctx, http.MethodGet, "http://fixture.invalid/events", http.NoBody)
	if err != nil {
		t.Fatal(err)
	}
	if lastEventID != "" {
		r.Header.Set("Last-Event-ID", lastEventID)
	}
	return r
}

func ccClient(rt http.RoundTripper, backoff sse.Backoff) *sse.Client {
	if backoff.InitialInterval <= 0 {
		backoff.InitialInterval = time.Nanosecond
	}
	return &sse.Client{
		HTTPClient:        &http.Client{Transport: rt},
		ResponseValidator: sse.NoopValidator,
		Backoff:           backoff,
	}
}

// ccDelivery is an event handed to the Consume handler along with its
// acknowledgement function.
type ccDelivery struct {
	event sse.Event
	ack   func() bool
}

func ccHandler(out chan<- ccDelivery, delivered *atomic.Int64) func(sse.Event, func() bool) {
	return func(e sse.Event, ack func() bool) {
		delivered.Add(1)
		out <- ccDelivery{event: e, ack: ack}
	}
}

func ccWrite(t *testing.T, w io.Writer, data string) {
	t.Helper()
	if _, err := io.WriteString(w, data); err != nil {
		t.Errorf("failed to write event: %v", err)
	}
}

func ccNext(c *taskcheck.C, ch <-chan ccDelivery, what string) (ccDelivery, bool) {
	select {
	case d := <-ch:
		return d, true
	case <-time.After(3 * time.Second):
		c.Check(false, "timed out waiting for %s", what)
		return ccDelivery{}, false
	}
}

func ccNone(c *taskcheck.C, ch <-chan ccDelivery, what string) {
	select {
	case d := <-ch:
		c.Check(false, "unexpected extra delivery for %s: %+v", what, d.event)
	case <-time.After(100 * time.Millisecond):
		c.Check(true, "no extra delivery for %s", what)
	}
}

func ccWaitErr(c *taskcheck.C, ch <-chan error, what string) (error, bool) {
	select {
	case err := <-ch:
		return err, true
	case <-time.After(3 * time.Second):
		c.Check(false, "timed out waiting for %s", what)
		return nil, false
	}
}

// ccWaitCalls waits until the transport has been used at least n times.
func ccWaitCalls(c *taskcheck.C, rt *ccTransport, n int) (ccHeader, bool) {
	deadline := time.Now().Add(3 * time.Second)
	for {
		calls, headers := rt.state()
		if calls >= n {
			return headers[n-1], true
		}
		if time.Now().After(deadline) {
			c.Check(false, "timed out waiting for HTTP attempt %d, only %d attempts were made", n, calls)
			return ccHeader{}, false
		}
		time.Sleep(2 * time.Millisecond)
	}
}

// ccAttempts serves bodies in order, but stops waiting when the request context
// ends. This keeps cancellation deterministic while a test controls an attempt.
func ccAttempts(bodies <-chan *ccBody) func(int, *http.Request) (*http.Response, error) {
	return func(_ int, r *http.Request) (*http.Response, error) {
		select {
		case body := <-bodies:
			return ccResponse(r, body), nil
		case <-r.Context().Done():
			return nil, r.Context().Err()
		}
	}
}

type ccPipe struct {
	reader *ccBody
	writer *io.PipeWriter
}

func ccNewPipe() ccPipe {
	pr, pw := io.Pipe()
	return ccPipe{reader: ccNewBody(pr), writer: pw}
}

// TestConsumePrefix checks that the delivery window counts deliveries that have
// not crossed the contiguous acknowledgement prefix, that out-of-order
// acknowledgements cannot release the gap and that acknowledgement functions
// are single-use and stop working when the delivery epoch ends.
func TestConsumePrefix(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx, cancel := context.WithCancel(context.Background())
		defer cancel()

		pipe := ccNewPipe()

		rt := &ccTransport{attempts: func(_ int, r *http.Request) (*http.Response, error) {
			return ccResponse(r, pipe.reader), nil
		}}

		client := ccClient(rt, sse.Backoff{MaxRetries: -1})

		conn := client.NewConnection(ccRequest(t, ctx, ""))
		deliveries := make(chan ccDelivery, 32)
		var delivered atomic.Int64
		consumeErr := make(chan error, 1)
		go func() {
			consumeErr <- conn.Consume(2, ccHandler(deliveries, &delivered))
		}()

		ccWrite(t, pipe.writer, "id: 1\ndata: one\n\n")
		ccWrite(t, pipe.writer, "id: 2\ndata: two\n\n")

		first, ok := ccNext(c, deliveries, "first event")
		if !ok {
			return
		}
		second, ok := ccNext(c, deliveries, "second event")
		if !ok {
			return
		}

		c.Check(first.event.LastEventID == "1" && first.event.Data == "one", "first event was %+v", first.event)
		c.Check(second.event.LastEventID == "2" && second.event.Data == "two", "second event was %+v", second.event)
		c.Check(delivered.Load() == 2, "expected exactly the window size to be delivered, got %d", delivered.Load())

		// Acknowledge the second delivery first: the gap of the first one must not
		// be released, so the window stays full and a third event cannot arrive.
		c.Check(second.ack(), "first acknowledgement of the second event must return true")
		ccNone(c, deliveries, "window saturation after out-of-order acknowledgement")
		c.Check(delivered.Load() == 2, "out-of-order acknowledgement released the window, %d deliveries", delivered.Load())
		c.Check(!second.ack(), "acknowledging an event twice must return false")

		// Acknowledging the first delivery commits both and frees the window.
		c.Check(first.ack(), "first acknowledgement of the first event must return true")
		ccWrite(t, pipe.writer, "id: 3\ndata: three\n\n")
		third, ok := ccNext(c, deliveries, "third event after the prefix was acknowledged")
		if !ok {
			return
		}
		c.Check(third.event.LastEventID == "3" && third.event.Data == "three", "third event was %+v", third.event)
		c.Check(third.ack(), "first acknowledgement of the third event must return true")
		c.Check(!third.ack(), "acknowledging the third event again must return false")

		ccWrite(t, pipe.writer, "id: 4\ndata: four\n\n")
		fourth, ok := ccNext(c, deliveries, "fourth event")
		if !ok {
			return
		}
		c.Check(fourth.event.LastEventID == "4", "fourth event has the wrong ID: %+v", fourth.event)
		c.Check(fourth.ack(), "first acknowledgement of the fourth event must return true")

		// Leave the fifth event unacknowledged and lose the connection: its
		// acknowledgement must be rejected once the attempt ends.
		ccWrite(t, pipe.writer, "id: 5\ndata: five\n\n")
		fifth, ok := ccNext(c, deliveries, "fifth event")
		if !ok {
			return
		}
		c.Check(fifth.event.LastEventID == "5", "fifth event has the wrong ID: %+v", fifth.event)
		_ = pipe.writer.Close()

		err, ok := ccWaitErr(c, consumeErr, "Consume to return after EOF")
		if !ok {
			return
		}
		c.Check(errors.Is(err, io.EOF), "expected the wrapped EOF error, got %v", err)
		c.Check(!fifth.ack(), "an acknowledgement left over after Consume returned must return false")
		c.Check(!fifth.ack(), "a second late acknowledgement must also return false")
		c.Check(delivered.Load() == 5, "unexpected number of deliveries: %d", delivered.Load())

		_, headers := rt.state()
		c.Check(len(headers) == 1, "expected a single HTTP attempt, got %d", len(headers))
		c.Check(pipe.reader.closeCount() >= 1, "response body was not closed")
	})
}

// TestConsumeEpoch checks reconnection semantics: the contiguous acknowledged
// prefix is the only cursor preserved, all other deliveries and acknowledgement
// functions are invalidated, and the caller-provided Last-Event-ID is retained
// until something is acknowledged.
func TestConsumeEpoch(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("cursor_and_epoch_invalidation", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			bodies := make(chan *ccBody, 4)
			rt := &ccTransport{attempts: ccAttempts(bodies)}

			client := ccClient(rt, sse.Backoff{})

			conn := client.NewConnection(ccRequest(t, ctx, "seed"))
			deliveries := make(chan ccDelivery, 32)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(2, ccHandler(deliveries, &delivered))
			}()

			attempt1 := ccNewPipe()
			bodies <- attempt1.reader

			ccWrite(t, attempt1.writer, "id: a\ndata: a\n\n")
			ccWrite(t, attempt1.writer, "id: b\ndata: b\n\n")

			eventA, ok := ccNext(c, deliveries, "event a")
			if !ok {
				return
			}
			eventB, ok := ccNext(c, deliveries, "event b")
			if !ok {
				return
			}
			c.Check(eventA.ack(), "event a must be acknowledged")
			c.Check(!eventA.ack(), "event a must not be acknowledged twice")
			c.Check(delivered.Load() == 2, "unexpected deliveries before reconnecting: %d", delivered.Load())

			// Disconnect and start the second attempt. The acknowledged prefix ends
			// at "a", so that is the cursor of the new request; b was never
			// acknowledged and must not be preserved.
			attempt2 := ccNewPipe()
			_ = attempt1.writer.Close()
			bodies <- attempt2.reader

			header2, ok := ccWaitCalls(c, rt, 2)
			if !ok {
				return
			}
			c.Check(header2.value == "a" && header2.present, "second attempt sent Last-Event-ID %q (present %v), expected %q", header2.value, header2.present, "a")
			c.Check(!eventB.ack(), "acknowledgement of the previous attempt's event must return false after reconnecting")

			// An event without an ID inherits the parser's cursor, which now is the
			// last contiguously acknowledged ID. Because it does not carry an ID,
			// it cannot move the cursor past "a" on its own, even when acknowledged.
			ccWrite(t, attempt2.writer, "data: inherited\n\n")
			inherited, ok := ccNext(c, deliveries, "event without ID")
			if !ok {
				return
			}
			c.Check(inherited.event.LastEventID == "a", "event without ID inherited %q, expected %q", inherited.event.LastEventID, "a")
			c.Check(inherited.ack(), "the inherited event must be acknowledged")

			ccWrite(t, attempt2.writer, "id: c\ndata: c\n\n")
			eventC, ok := ccNext(c, deliveries, "event c")
			if !ok {
				return
			}
			c.Check(eventC.event.LastEventID == "c", "event c has the wrong ID: %+v", eventC.event)
			c.Check(eventC.ack(), "event c must be acknowledged once")
			c.Check(!eventC.ack(), "a repeated acknowledgement of event c must be rejected")
			c.Check(!inherited.ack(), "a repeated acknowledgement of the inherited event must be rejected")

			// The third attempt must resume at c: both the inherited event and
			// c are acknowledged, and the inherited event's ID is the same as the
			// last authenticated cursor value.
			attempt3 := ccNewPipe()
			_ = attempt2.writer.Close()
			bodies <- attempt3.reader

			header3, ok := ccWaitCalls(c, rt, 3)
			if !ok {
				return
			}
			c.Check(header3.value == "c" && header3.present, "third attempt sent Last-Event-ID %q (present %v), expected %q", header3.value, header3.present, "c")

			_ = attempt3.writer.Close()
			cancel()

			err, ok := ccWaitErr(c, consumeErr, "Consume to stop on context cancellation")
			if !ok {
				return
			}
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
			c.Check(!eventB.ack(), "acknowledgement from the first epoch must stay invalid")
			c.Check(!eventC.ack(), "acknowledgement from the second epoch must be invalid once Consume returns")
			c.Check(attempt1.reader.closeCount() >= 1, "attempt 1 body was not closed")
			c.Check(attempt2.reader.closeCount() >= 1, "attempt 2 body was not closed")
			c.Check(attempt3.reader.closeCount() >= 1, "attempt 3 body was not closed")
		})

		t.Run("ack_with_disconnect_concurrent", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			bodies := make(chan *ccBody, 4)
			rt := &ccTransport{attempts: ccAttempts(bodies)}

			client := ccClient(rt, sse.Backoff{})

			conn := client.NewConnection(ccRequest(t, ctx, ""))
			deliveries := make(chan ccDelivery, 8)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(2, ccHandler(deliveries, &delivered))
			}()

			attempt1 := ccNewPipe()
			bodies <- attempt1.reader

			ccWrite(t, attempt1.writer, "id: a\ndata: a\n\n")
			ccWrite(t, attempt1.writer, "id: b\ndata: b\n\n")
			eventA, ok := ccNext(c, deliveries, "event a")
			if !ok {
				return
			}
			eventB, ok := ccNext(c, deliveries, "event b")
			if !ok {
				return
			}
			c.Check(eventA.ack(), "event a must be acknowledged")

			// Race the acknowledgement of b against the disconnection.
			ackDone := make(chan bool, 1)
			go func() { ackDone <- eventB.ack() }()
			_ = attempt1.writer.Close()

			attempt2 := ccNewPipe()
			_ = attempt2.writer.Close()
			bodies <- attempt2.reader

			accepted := <-ackDone
			header2, ok := ccWaitCalls(c, rt, 2)
			if !ok {
				return
			}

			// If the acknowledgement won the race the cursor is b; if the
			// disconnect won, the acknowledgement must have been rejected and the
			// cursor must still be a. No other combination is consistent.
			if accepted {
				c.Check(header2.value == "b" && header2.present, "acknowledgement won the race but Last-Event-ID is %q (present %v), expected b", header2.value, header2.present)
			} else {
				c.Check(header2.value == "a" && header2.present, "acknowledgement lost the race but Last-Event-ID is %q (present %v), expected a", header2.value, header2.present)
			}

			cancel()
			err, ok := ccWaitErr(c, consumeErr, "Consume to stop")
			if !ok {
				return
			}
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
			c.Check(!eventB.ack(), "a late acknowledgement after the race must be rejected")
			c.Check(attempt1.reader.closeCount() >= 1, "attempt 1 body was not closed")
			c.Check(attempt2.reader.closeCount() >= 1, "attempt 2 body was not closed")
		})

		t.Run("no_ack_retains_caller_id", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			attempt1 := ccNewPipe()
			attempt2 := ccNewPipe()

			rt := &ccTransport{attempts: func(call int, r *http.Request) (*http.Response, error) {
				if call == 0 {
					return ccResponse(r, attempt1.reader), nil
				}
				return ccResponse(r, attempt2.reader), nil
			}}
			// Infinite retries: the second attempt stays open until we cancel.
			client := ccClient(rt, sse.Backoff{MaxRetries: 0})

			conn := client.NewConnection(ccRequest(t, ctx, "seed"))
			deliveries := make(chan ccDelivery, 8)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(1, ccHandler(deliveries, &delivered))
			}()

			ccWrite(t, attempt1.writer, "id: a\ndata: a\n\n")
			if _, ok := ccNext(c, deliveries, "unacknowledged event a"); !ok {
				return
			}
			_ = attempt1.writer.Close()

			header2, ok := ccWaitCalls(c, rt, 2)
			if !ok {
				return
			}
			c.Check(header2.value == "seed" && header2.present, "without acknowledgements the caller Last-Event-ID must be retained, got %q (present %v)", header2.value, header2.present)

			cancel()
			err, ok := ccWaitErr(c, consumeErr, "Consume to stop")
			if !ok {
				return
			}
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
		})

		t.Run("ack_empty_id_removes_header", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			attempt1 := ccNewPipe()
			attempt2 := ccNewPipe()

			rt := &ccTransport{attempts: func(call int, r *http.Request) (*http.Response, error) {
				if call == 0 {
					return ccResponse(r, attempt1.reader), nil
				}
				return ccResponse(r, attempt2.reader), nil
			}}
			// Infinite retries: the second attempt stays open until we cancel.
			client := ccClient(rt, sse.Backoff{MaxRetries: 0})

			conn := client.NewConnection(ccRequest(t, ctx, "seed"))
			deliveries := make(chan ccDelivery, 8)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(1, ccHandler(deliveries, &delivered))
			}()

			ccWrite(t, attempt1.writer, "id:\ndata: empty id\n\n")
			emptyID, ok := ccNext(c, deliveries, "event with empty ID")
			if !ok {
				return
			}
			c.Check(emptyID.event.LastEventID == "", "event did not have an empty ID: %+v", emptyID.event)
			c.Check(emptyID.ack(), "the event with empty ID must be acknowledged")
			_ = attempt1.writer.Close()

			header2, ok := ccWaitCalls(c, rt, 2)
			if !ok {
				return
			}
			c.Check(!header2.present, "an acknowledged empty ID must remove Last-Event-ID, got %q (present %v)", header2.value, header2.present)

			cancel()
			err, ok := ccWaitErr(c, consumeErr, "Consume to stop")
			if !ok {
				return
			}
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
		})
	})
}

// TestConsumeCompatibility checks that the legacy subscription API is unaffected
// and that Consume shares the HTTP behavior of Connect: argument validation
// happens before any request, the configured buffer is used, empty-data events
// are delivered, and context cancellation ends reads, window waits and retry
// waits with a recognizable context error.
func TestConsumeCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("legacy_subscriptions", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			var events []string
			second := ccNewPipe()
			rt := &ccTransport{attempts: func(call int, r *http.Request) (*http.Response, error) {
				if call == 0 {
					return ccResponse(r, ccNewBody(io.NopCloser(strings.NewReader("id: alpha\ndata: one\n\nid: beta\ndata: two\n\n")))), nil
				}
				return ccResponse(r, second.reader), nil
			}}
			client := ccClient(rt, sse.Backoff{MaxRetries: 0})

			conn := client.NewConnection(ccRequest(t, ctx, ""))
			conn.SubscribeToAll(func(e sse.Event) {
				events = append(events, e.LastEventID+":"+e.Data)
			})
			connectErr := make(chan error, 1)
			go func() { connectErr <- conn.Connect() }()

			header2, ok := ccWaitCalls(c, rt, 2)
			if !ok {
				return
			}
			// The legacy path updates the cursor as soon as the callback is called,
			// without any explicit acknowledgement.
			c.Check(header2.value == "beta" && header2.present, "legacy cursor is %q (present %v), expected beta", header2.value, header2.present)

			select {
			case <-time.After(50 * time.Millisecond):
				c.Check(true, "legacy connection stayed open for the second attempt")
			case err := <-connectErr:
				c.Check(false, "Connect returned early: %v", err)
			}

			cancel()
			// The legacy read consumes the pipe body, so cancel alone may not end
			// it; close the attempt's body too, like a dropped connection would.
			_ = second.writer.Close()
			err, ok := ccWaitErr(c, connectErr, "Connect to return")
			if !ok {
				return
			}
			c.Check(err != nil, "expected a connection error, got %v", err)
			c.Check(len(events) == 2 && events[0] == "alpha:one" && events[1] == "beta:two", "legacy callback delivery changed: %q", events)
		})

		t.Run("consume_owns_delivery", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			pipe := ccNewPipe()
			rt := &ccTransport{attempts: func(_ int, r *http.Request) (*http.Response, error) {
				return ccResponse(r, pipe.reader), nil
			}}
			client := ccClient(rt, sse.Backoff{MaxRetries: -1})
			conn := client.NewConnection(ccRequest(t, ctx, ""))

			var subscribed atomic.Int64
			conn.SubscribeToAll(func(sse.Event) { subscribed.Add(1) })

			deliveries := make(chan ccDelivery, 4)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(1, ccHandler(deliveries, &delivered))
			}()

			ccWrite(t, pipe.writer, "id: 1\ndata: via consume\n\n")
			event, ok := ccNext(c, deliveries, "event via Consume")
			if !ok {
				return
			}
			c.Check(event.ack(), "the event must be acknowledged")
			_ = pipe.writer.Close()

			err, ok := ccWaitErr(c, consumeErr, "Consume to return")
			if !ok {
				return
			}
			c.Check(errors.Is(err, io.EOF), "expected the wrapped EOF error, got %v", err)
			c.Check(subscribed.Load() == 0, "Consume must not invoke subscription callbacks, got %d calls", subscribed.Load())
			c.Check(delivered.Load() == 1, "Consume delivered %d events, expected 1", delivered.Load())
		})

		t.Run("validation_before_http", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			rt := &ccTransport{attempts: func(_ int, r *http.Request) (*http.Response, error) {
				return ccResponse(r, ccNewBody(io.NopCloser(strings.NewReader("")))), nil
			}}
			client := ccClient(rt, sse.Backoff{MaxRetries: -1})
			conn := client.NewConnection(ccRequest(t, ctx, ""))

			var delivered atomic.Int64
			drain := func(_ sse.Event, ack func() bool) {
				delivered.Add(1)
				_ = ack()
			}

			c.Check(conn.Consume(0, drain) != nil, "a zero window must be rejected")
			c.Check(conn.Consume(-3, drain) != nil, "a negative window must be rejected")
			c.Check(conn.Consume(1, nil) != nil, "a nil handler must be rejected")

			_, headers := rt.state()
			c.Check(len(headers) == 0, "invalid Consume arguments sent %d HTTP requests", len(headers))
			c.Check(delivered.Load() == 0, "invalid Consume arguments delivered %d events", delivered.Load())
		})

		t.Run("buffer_and_empty_data", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			big := strings.Repeat("x", 128*1024)
			pipe := ccNewPipe()
			rt := &ccTransport{attempts: func(_ int, r *http.Request) (*http.Response, error) {
				return ccResponse(r, pipe.reader), nil
			}}
			client := ccClient(rt, sse.Backoff{MaxRetries: -1})
			conn := client.NewConnection(ccRequest(t, ctx, ""))
			conn.Buffer(nil, 256*1024)

			deliveries := make(chan ccDelivery, 8)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(1, ccHandler(deliveries, &delivered))
			}()

			ccWrite(t, pipe.writer, "id: big\ndata: "+big+"\n\n")
			huge, ok := ccNext(c, deliveries, "large event")
			if !ok {
				return
			}
			c.Check(huge.event.Data == big && huge.event.LastEventID == "big", "large event was truncated: %d bytes, ID %q", len(huge.event.Data), huge.event.LastEventID)
			c.Check(huge.ack(), "the large event must be acknowledged")

			// An event with an empty data field is produced by the parser and must
			// be delivered and acknowledgeable like any other event.
			ccWrite(t, pipe.writer, "id: empty\ndata:\n\n")
			emptyData, ok := ccNext(c, deliveries, "empty data event")
			if !ok {
				return
			}
			c.Check(emptyData.event.Data == "" && emptyData.event.LastEventID == "empty", "empty-data event has unexpected contents: %+v", emptyData.event)
			c.Check(emptyData.ack(), "the empty-data event must be acknowledged and confirmable")

			_ = pipe.writer.Close()
			err, ok := ccWaitErr(c, consumeErr, "Consume to return")
			if !ok {
				return
			}
			c.Check(errors.Is(err, io.EOF), "expected the wrapped EOF error, got %v", err)
			c.Check(pipe.reader.closeCount() >= 1, "response body was not closed")
		})

		t.Run("context_ends_window_wait", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			pipe := ccNewPipe()
			rt := &ccTransport{attempts: func(_ int, r *http.Request) (*http.Response, error) {
				return ccResponse(r, pipe.reader), nil
			}}
			client := ccClient(rt, sse.Backoff{MaxRetries: -1})
			conn := client.NewConnection(ccRequest(t, ctx, ""))

			deliveries := make(chan ccDelivery, 8)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(1, ccHandler(deliveries, &delivered))
			}()

			ccWrite(t, pipe.writer, "id: 1\ndata: first\n\n")
			first, ok := ccNext(c, deliveries, "first event")
			if !ok {
				return
			}

			// The parser may read ahead into its buffer, but the second event must
			// not be delivered while the window is full.
			go func() { _, _ = io.WriteString(pipe.writer, "id: 2\ndata: second\n\n") }()
			ccNone(c, deliveries, "window saturation")

			cancel()

			err, ok := ccWaitErr(c, consumeErr, "Consume to return after context cancellation")
			if !ok {
				return
			}
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
			c.Check(!first.ack(), "acknowledgements must be invalid after Consume returns")
			c.Check(delivered.Load() == 1, "a blocked window wait still delivered %d events", delivered.Load())
			c.Check(pipe.reader.closeCount() >= 1, "context cancellation did not close the response body")
		})

		t.Run("context_ends_retry_wait", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			rt := &ccTransport{attempts: func(_ int, r *http.Request) (*http.Response, error) {
				return ccResponse(r, ccNewBody(io.NopCloser(strings.NewReader("")))), nil
			}}
			client := ccClient(rt, sse.Backoff{InitialInterval: 50 * time.Millisecond, MaxRetries: 0})
			client.OnRetry = func(_ error, _ time.Duration) {
				cancel()
			}
			conn := client.NewConnection(ccRequest(t, ctx, ""))

			deliveries := make(chan ccDelivery, 4)
			var delivered atomic.Int64
			err := conn.Consume(1, ccHandler(deliveries, &delivered))
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled from the retry wait, got %v", err)

			_, headers := rt.state()
			c.Check(len(headers) >= 1, "no attempt was made")
			c.Check(delivered.Load() == 0, "unexpected delivery during an empty stream: %d", delivered.Load())
		})

		t.Run("context_cause_is_recognizable", func(t *testing.T) {
			cause := errors.New("custom cause")
			ctx, cancel := context.WithCancelCause(context.Background())
			defer cancel(nil)

			pipe := ccNewPipe()
			rt := &ccTransport{attempts: func(_ int, r *http.Request) (*http.Response, error) {
				return ccResponse(r, pipe.reader), nil
			}}
			client := ccClient(rt, sse.Backoff{MaxRetries: -1})
			conn := client.NewConnection(ccRequest(t, ctx, ""))

			blocked := make(chan func() bool, 1)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(1, func(_ sse.Event, ack func() bool) {
					delivered.Add(1)
					blocked <- ack
				})
			}()

			// Fill the window and leave the event unacknowledged, then cancel
			// with a cause while the parser is waiting for window space.
			ccWrite(t, pipe.writer, "id: 1\ndata: first\n\n")
			ack := <-blocked
			cancel(cause)

			err, ok := ccWaitErr(c, consumeErr, "Consume to return after cancellation with a cause")
			if !ok {
				return
			}
			c.Check(errors.Is(err, ctx.Err()), "the returned error must match ctx.Err(); got %v", err)
			c.Check(errors.Is(err, context.Canceled), "the returned error must match context.Canceled; got %v", err)
			c.Check(!ack(), "acknowledgements must be invalid after Consume returns")
			c.Check(delivered.Load() == 1, "window cancellation delivered %d events", delivered.Load())
			_ = pipe.writer.Close()
		})

		t.Run("client_http_settings", func(t *testing.T) {
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()

			// A body with GetBody exercises request-body resetting on reconnect,
			// and the custom validator proves the Client's validator is used.
			firstAttempt := ccNewPipe()
			second := ccNewPipe()
			var calls atomic.Int64
			var validated atomic.Int64
			rt := &ccTransport{attempts: func(call int, r *http.Request) (*http.Response, error) {
				calls.Add(1)
				body, err := io.ReadAll(r.Body)
				if err != nil {
					return nil, err
				}
				if string(body) != "payload" {
					return nil, fmt.Errorf("request body was %q", body)
				}
				if call == 0 {
					return ccResponse(r, firstAttempt.reader), nil
				}
				return ccResponse(r, second.reader), nil
			}}
			client := ccClient(rt, sse.Backoff{MaxRetries: 0})
			client.ResponseValidator = func(res *http.Response) error {
				validated.Add(1)
				if res.StatusCode != http.StatusOK {
					return fmt.Errorf("unexpected status %d", res.StatusCode)
				}
				return nil
			}

			body := strings.NewReader("payload")
			req := ccRequest(t, ctx, "")
			req.Body = io.NopCloser(body)
			req.ContentLength = int64(len("payload"))
			req.GetBody = func() (io.ReadCloser, error) {
				return io.NopCloser(strings.NewReader("payload")), nil
			}
			conn := client.NewConnection(req)

			deliveries := make(chan ccDelivery, 8)
			var delivered atomic.Int64
			consumeErr := make(chan error, 1)
			go func() {
				consumeErr <- conn.Consume(1, ccHandler(deliveries, &delivered))
			}()

			ccWrite(t, firstAttempt.writer, "data: first\n\n")
			first, ok := ccNext(c, deliveries, "event from the first attempt")
			if !ok {
				return
			}
			c.Check(first.event.Data == "first", "unexpected first event: %+v", first.event)
			c.Check(first.ack(), "the first event must be acknowledged")

			_ = firstAttempt.writer.Close()
			_, _ = ccWaitCalls(c, rt, 2)
			c.Check(calls.Load() == 2, "expected two attempts, got %d", calls.Load())
			c.Check(validated.Load() == 2, "the client ResponseValidator ran %d times, expected 2", validated.Load())

			cancel()
			err, ok := ccWaitErr(c, consumeErr, "Consume to return")
			if !ok {
				return
			}
			c.Check(errors.Is(err, context.Canceled), "expected context.Canceled, got %v", err)
		})
	})
}
