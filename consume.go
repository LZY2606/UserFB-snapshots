package sse

import (
	"context"
	"errors"
	"net/http"
	"time"

	"github.com/tmaxmax/go-sse/internal/parser"
)

var (
	errConsumeWindow  = errors.New("go-sse: Consume window must be positive")
	errConsumeHandler = errors.New("go-sse: Consume handler cannot be nil")
	errConsumeClosed  = errors.New("go-sse: Consume delivery epoch ended")
)

// consumeDelivery is a single event delivered by Consume. Deliveries are identified
// by their position in the delivery sequence, not by their event ID; IDs are opaque
// and may repeat, be inherited or be empty.
type consumeDelivery struct {
	event Event
	acked bool
}

// consumeEpoch groups the deliveries of a single connection attempt. Every
// reconnection starts a new epoch: acknowledgement functions of previous epochs
// become invalid and cannot advance the cursor of the new one.
type consumeEpoch struct {
	events     []*consumeDelivery
	committed  int // number of deliveries at the front of events that are acknowledged
	parserBase string
}

// Consume starts the connection and delivers events one by one to handler, in
// parsing order. Unlike the Subscribe methods, the caller must explicitly
// acknowledge each event by calling the function passed to the handler. The
// connection's Last-Event-ID cursor only advances past a contiguous prefix of
// acknowledged deliveries; events acknowledged out of order cannot release a gap
// before them. Event IDs are treated as opaque values: they may repeat, be
// inherited by events without an ID, or be empty, and deliveries are ordered
// solely by their position in the stream.
//
// The window is the maximum number of delivered events that are not yet part of
// the contiguous acknowledged prefix. When it is full, parsing stops until the
// gap is acknowledged; Consume neither queues the remaining events nor spawns a
// goroutine per event. The handler runs on the parsing goroutine and must return
// promptly – it may hand the event and its acknowledgement function to another
// goroutine, but blocking in the handler stops delivery. The acknowledgement
// function returns true only on its first call for a delivery that still belongs
// to the current connection attempt; every later call, and every call after the
// attempt ends, returns false without advancing any cursor.
//
// Every reconnection starts a new attempt (epoch): the ID of the last
// contiguously acknowledged event is preserved and sent as Last-Event-ID, while
// all other deliveries and acknowledgement functions are invalidated. Events
// without an ID keep inheriting the parser's current ID, which for a new attempt
// is the preserved cursor. Before the first acknowledged event, the
// caller-provided Last-Event-ID is retained on reconnection; once an event with
// an empty ID is acknowledged, the header is removed. Unacknowledged events may
// therefore be replayed and are not deduplicated by ID.
//
// Consume uses the Client's HTTPClient, ResponseValidator, Backoff and OnRetry
// settings, and the request's GetBody, just like Connect: it shares the same
// attempt and retry machinery rather than implementing a separate one. The
// response body is closed after every attempt. When the request's context ends,
// in-flight reads, a full-window wait and a retry wait all stop, Consume returns
// an error for which errors.Is(err, ctx.Err()) is true, and every outstanding
// acknowledgement function becomes invalid. Consume does not interrupt handler
// code that has already been called.
//
// Consume owns event delivery for this Connection: it never invokes the
// callbacks registered with SubscribeMessages, SubscribeEvent or SubscribeToAll.
// Consume and the Subscribe methods must not be used on the same Connection.
//
// Invalid arguments (a non-positive window or a nil handler) are reported before
// any HTTP request is made.
func (c *Connection) Consume(window int, handler func(Event, func() bool)) error {
	if window <= 0 {
		return errConsumeWindow
	}
	if handler == nil {
		return errConsumeHandler
	}

	ctx := c.request.Context()

	c.mu.Lock()
	c.consumeWindow = window
	c.consumeHandler = handler
	c.consumeInitialID = c.request.Header.Get("Last-Event-ID")
	c.consumeAcked = false
	c.consumeLastAckedID = ""
	c.consumeEpoch = nil
	c.mu.Unlock()

	// Invalidate all outstanding acknowledgement functions when Consume returns.
	defer func() {
		c.mu.Lock()
		c.consumeEpoch = nil
		c.notifyConsumeWaitersLocked()
		c.mu.Unlock()
	}()

	c.request.Header.Set("Accept", "text/event-stream")
	c.request.Header.Set("Connection", "keep-alive")
	c.request.Header.Set("Cache", "no-cache")

	return c.retryLoop(ctx, func(setRetry func(time.Duration)) (bool, error) {
		epoch := c.beginConsumeEpoch()

		shouldRetry, err := c.runAttempt(ctx, c.resetConsumeRequest, setRetry, func(res *http.Response, setRetry func(time.Duration)) error {
			return c.consumeBody(ctx, epoch, res, setRetry)
		})

		// Invalidate this attempt's deliveries and acknowledgement functions
		// before any retry wait or return.
		c.endConsumeEpoch(epoch)

		return shouldRetry, err
	})
}

func (c *Connection) resetConsumeRequest() error {
	return c.resetRequestWith(func() (string, bool) {
		c.mu.Lock()
		defer c.mu.Unlock()
		return c.consumeLastAckedID, c.consumeAcked
	})
}

func (c *Connection) beginConsumeEpoch() *consumeEpoch {
	c.mu.Lock()
	defer c.mu.Unlock()

	base := c.consumeInitialID
	if c.consumeAcked {
		// Events without an ID inherited the ID of the position the attempt
		// started from; continue inheriting from the last committed ID.
		base = c.consumeLastAckedID
	}

	epoch := &consumeEpoch{parserBase: base}
	c.consumeEpoch = epoch

	return epoch
}

func (c *Connection) endConsumeEpoch(epoch *consumeEpoch) {
	c.mu.Lock()
	if c.consumeEpoch == epoch {
		c.consumeEpoch = nil
	}
	// Release the attempt's deliveries: outstanding acknowledgement functions
	// keep working (they return false) but no longer pin the event payloads.
	epoch.events = nil
	c.notifyConsumeWaitersLocked()
	c.mu.Unlock()
}

// notifyConsumeWaitersLocked wakes every goroutine waiting for window space.
// Closing a channel can't lose a wakeup that happens between a waiter's check
// and its wait, and each waiter replaces the channel before releasing the lock.
func (c *Connection) notifyConsumeWaitersLocked() {
	close(c.consumeNotify)
	c.consumeNotify = make(chan struct{})
}

// ackConsume acknowledges a delivery. It returns true only if the delivery belongs
// to the current attempt and was not acknowledged before. Acknowledging an event
// commits it as soon as all deliveries before it are acknowledged; out-of-order
// acknowledgements do not release the gap.
func (c *Connection) ackConsume(epoch *consumeEpoch, delivery *consumeDelivery) bool {
	c.mu.Lock()
	defer c.mu.Unlock()

	if c.consumeEpoch != epoch || delivery.acked {
		return false
	}
	delivery.acked = true

	advanced := false
	for epoch.committed < len(epoch.events) && epoch.events[epoch.committed].acked {
		c.consumeAcked = true
		c.consumeLastAckedID = epoch.events[epoch.committed].event.LastEventID
		epoch.committed++
		advanced = true
	}

	// Drop the committed prefix once it is large enough to be worth the copy.
	// Deliveries handed out earlier stay referenced by their acknowledgement
	// closures; they only become unreachable together with the epoch, so this
	// bounds the retained event payloads to the uncommitted window plus the
	// already-delivered prefix of the current attempt.
	if epoch.committed > 0 && epoch.committed >= len(epoch.events)/2 {
		epoch.events = epoch.events[epoch.committed:]
		epoch.committed = 0
	}

	if advanced || len(epoch.events)-epoch.committed < c.consumeWindow {
		c.notifyConsumeWaitersLocked()
	}

	return true
}

func (c *Connection) consumeBody(ctx context.Context, epoch *consumeEpoch, res *http.Response, setRetry func(time.Duration)) error {
	// A blocked body read must end when the request context ends. Closing the
	// body unblocks transports that don't observe the request context themselves.
	// The attempt closes it too; the onceCloser wrapper makes that a no-op.
	stop := make(chan struct{})
	go func() {
		select {
		case <-ctx.Done():
			_ = res.Body.Close()
		case <-stop:
		}
	}()
	defer close(stop)

	pf := func() *parser.Parser { return c.parser(res.Body) }

	var readErr error
	read(pf, epoch.parserBase, func(r int64) { setRetry(time.Duration(r) * time.Millisecond) }, false)(func(e Event, err error) bool {
		if err != nil {
			readErr = err
			return false
		}
		if err := c.deliverConsume(ctx, epoch, e); err != nil {
			readErr = err
			return false
		}
		return true
	})

	// A context that ended while the stream did must always be reported as such.
	if err := ctx.Err(); err != nil {
		return err
	}

	return readErr
}

func (c *Connection) deliverConsume(ctx context.Context, epoch *consumeEpoch, e Event) error {
	c.mu.Lock()
	for {
		if c.consumeEpoch != epoch {
			c.mu.Unlock()
			if err := ctx.Err(); err != nil {
				return err
			}
			// The attempt was torn down while this delivery waited for window
			// space. The event is dropped: unacknowledged events are replayed.
			return errConsumeClosed
		}

		if err := ctx.Err(); err != nil {
			c.mu.Unlock()
			return err
		}

		if len(epoch.events)-epoch.committed < c.consumeWindow {
			break
		}

		notify := c.consumeNotify
		c.mu.Unlock()

		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-notify:
		}

		c.mu.Lock()
	}

	delivery := &consumeDelivery{event: e}
	epoch.events = append(epoch.events, delivery)
	handler := c.consumeHandler
	c.mu.Unlock()

	handler(e, func() bool { return c.ackConsume(epoch, delivery) })

	return nil
}
