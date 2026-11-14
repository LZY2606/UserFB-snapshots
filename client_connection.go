package sse

import (
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"sync"
	"time"

	"github.com/tmaxmax/go-sse/internal/parser"
)

// EventCallback is a function that is used to receive events from a Connection.
type EventCallback func(Event)

// EventCallbackRemover is a function that removes an already registered callback
// from a connection. Calling it multiple times is a no-op.
type EventCallbackRemover func()

// Connection is a connection to an events stream. Created using the Client struct,
// a Connection processes the incoming events and calls the subscribed event callbacks.
// If the connection to the server temporarily fails, the connection will be reattempted.
// Retry values received from servers will be taken into account.
//
// Connections must not be copied after they are created.
type Connection struct { //nolint:govet // The current order aids readability.
	mu           sync.RWMutex
	request      *http.Request
	callbacks    map[string]map[int]EventCallback
	callbacksAll map[int]EventCallback
	lastEventID  string
	client       Client
	buf          []byte
	bufMaxSize   int
	callbackID   int
	isRetry      bool

	// State used by Consume.
	consumeNotify      chan struct{}
	consumeHandler     func(Event, func() bool)
	consumeEpoch       *consumeEpoch
	consumeInitialID   string
	consumeLastAckedID string
	consumeWindow      int
	consumeAcked       bool
}

// SubscribeMessages subscribes the given callback to all events without type (without or with empty `event` field).
// Remove the callback by calling the returned function.
func (c *Connection) SubscribeMessages(cb EventCallback) EventCallbackRemover {
	return c.SubscribeEvent("", cb)
}

// SubscribeEvent subscribes the given callback to all the events with the provided type
// (the `event` field has the value given here).
// Remove the callback by calling the returned function.
func (c *Connection) SubscribeEvent(typ string, cb EventCallback) EventCallbackRemover {
	return c.addSubscriber(typ, cb)
}

// SubscribeToAll subscribes the given callback to all events, with or without type.
// Remove the callback by calling the returned function.
func (c *Connection) SubscribeToAll(cb EventCallback) EventCallbackRemover {
	return c.addSubscriberToAll(cb)
}

func (c *Connection) addSubscriberToAll(cb EventCallback) EventCallbackRemover {
	c.mu.Lock()
	defer c.mu.Unlock()

	id := c.callbackID
	c.callbacksAll[id] = cb
	c.callbackID++

	return func() {
		c.mu.Lock()
		defer c.mu.Unlock()

		delete(c.callbacksAll, id)
	}
}

func (c *Connection) addSubscriber(event string, cb EventCallback) EventCallbackRemover {
	c.mu.Lock()
	defer c.mu.Unlock()

	if _, ok := c.callbacks[event]; !ok {
		c.callbacks[event] = map[int]EventCallback{}
	}

	id := c.callbackID
	c.callbacks[event][id] = cb
	c.callbackID++

	return func() {
		c.mu.Lock()
		defer c.mu.Unlock()

		delete(c.callbacks[event], id)
		if len(c.callbacks[event]) == 0 {
			delete(c.callbacks, event)
		}
	}
}

// Buffer sets the underlying buffer to be used when scanning events.
// Use this if you need to read very large events (bigger than the default
// of 65K bytes).
//
// Read the documentation of bufio.Scanner.Buffer for more information.
func (c *Connection) Buffer(buf []byte, maxSize int) {
	c.buf = buf
	c.bufMaxSize = maxSize
}

// ConnectionError is the type that wraps all the connection errors that occur.
type ConnectionError struct {
	// The request for which the connection failed.
	Req *http.Request
	// The reason the operation failed.
	Err error
	// The reason why the request failed.
	Reason string
}

func (e *ConnectionError) Error() string {
	return fmt.Sprintf("request failed: %s: %v", e.Reason, e.Err)
}

func (e *ConnectionError) Unwrap() error {
	return e.Err
}

func (c *Connection) resetRequest() error {
	return c.resetRequestWith(func() (string, bool) { return c.lastEventID, true })
}

// resetRequestWith resets the request body and the Last-Event-ID header before
// a reconnection attempt. The lastID function returns the ID to send and whether
// the header should be updated at all.
func (c *Connection) resetRequestWith(lastID func() (string, bool)) error {
	if !c.isRetry {
		c.isRetry = true
		return nil
	}
	if err := resetRequestBody(c.request); err != nil {
		return err
	}
	id, update := lastID()
	if !update {
		return nil
	}
	if id == "" {
		c.request.Header.Del("Last-Event-ID")
	} else {
		c.request.Header.Set("Last-Event-ID", id)
	}
	return nil
}

func (c *Connection) dispatch(ev Event) {
	c.mu.RLock()
	defer c.mu.RUnlock()

	cbs := c.callbacks[ev.Type]
	cbCount := len(cbs) + len(c.callbacksAll)
	if cbCount == 0 {
		return
	}

	for _, cb := range c.callbacks[ev.Type] {
		cb(ev)
	}
	for _, cb := range c.callbacksAll {
		cb(ev)
	}
}

func (c *Connection) parser(r io.Reader) *parser.Parser {
	p := parser.New(r)
	if c.buf != nil || c.bufMaxSize > 0 {
		p.Buffer(c.buf, c.bufMaxSize)
	}
	return p
}

func (c *Connection) read(r io.Reader, setRetry func(time.Duration)) error {
	pf := func() *parser.Parser { return c.parser(r) }

	var readErr error
	read(pf, c.lastEventID, func(r int64) { setRetry(time.Duration(r) * time.Millisecond) }, false)(func(e Event, err error) bool {
		if err != nil {
			readErr = err
			return false
		}
		c.lastEventID = e.LastEventID
		c.dispatch(e)
		return true
	})

	return readErr
}

// Connect sends the request the connection was created with to the server
// and, if successful, it starts receiving events. The caller goroutine
// is blocked until the request's context is done or an error occurs.
//
// If the request's context is cancelled, Connect returns its error.
// Otherwise, if the maximum number of retries is made, the last error
// that occurred is returned. Connect never returns otherwise – either
// the context is cancelled, or it's done retrying.
//
// All errors returned other than the context errors will be wrapped
// inside a *ConnectionError.
func (c *Connection) Connect() error {
	ctx := c.request.Context()

	c.request.Header.Set("Accept", "text/event-stream")
	c.request.Header.Set("Connection", "keep-alive")
	c.request.Header.Set("Cache", "no-cache")

	return c.retryLoop(ctx, func(setRetry func(time.Duration)) (bool, error) {
		return c.doConnect(ctx, setRetry)
	})
}

// retryLoop runs the connection attempts according to the client's backoff
// configuration. It is shared by Connect and Consume.
func (c *Connection) retryLoop(ctx context.Context, attempt func(setRetry func(time.Duration)) (bool, error)) error {
	backoff := c.client.Backoff.new()

	t := time.NewTimer(0)
	defer t.Stop()

	for {
		select {
		case <-t.C:
			shouldRetry, err := attempt(backoff.reset)
			if !shouldRetry {
				return err
			}

			next, shouldRetry := backoff.next()
			if !shouldRetry {
				return err
			}

			if c.client.OnRetry != nil {
				c.client.OnRetry(err, next)
			}

			t.Reset(next)
		case <-ctx.Done():
			return ctx.Err()
		}
	}
}

func (c *Connection) doConnect(ctx context.Context, setRetry func(time.Duration)) (bool, error) {
	return c.runAttempt(ctx, c.resetRequest, setRetry, func(res *http.Response, setRetry func(time.Duration)) error {
		return c.read(res.Body, setRetry)
	})
}

// runAttempt executes a single HTTP request attempt, shared by Connect and Consume.
// The body is always closed after the attempt. The read function handles the
// response body; it returns the error that ended the attempt.
func (c *Connection) runAttempt(ctx context.Context, reset func() error, setRetry func(time.Duration), readBody func(*http.Response, func(time.Duration)) error) (shouldRetry bool, err error) {
	if err := reset(); err != nil {
		return false, &ConnectionError{Req: c.request, Reason: "request reset failed", Err: err}
	}

	res, err := c.client.HTTPClient.Do(c.request)
	if err != nil {
		concrete := err.(*url.Error) //nolint:errorlint // We know the concrete type here
		if errors.Is(err, ctx.Err()) {
			return false, concrete.Err
		}
		return true, &ConnectionError{Req: c.request, Reason: "connection to server failed", Err: concrete.Err}
	}
	res.Body = &onceCloser{ReadCloser: res.Body}
	defer res.Body.Close()

	if err := c.client.ResponseValidator(res); err != nil {
		return false, &ConnectionError{Req: c.request, Reason: "response validation failed", Err: err}
	}

	setRetry(0)

	err = readBody(res, setRetry)
	if errors.Is(err, ctx.Err()) {
		return false, err
	}

	return true, &ConnectionError{Req: c.request, Reason: "connection to server lost", Err: err}
}

// onceCloser guarantees that the response body is closed only once, even when
// both the read path and the attempt teardown try to close it.
type onceCloser struct {
	io.ReadCloser
	once sync.Once
	err  error
}

func (c *onceCloser) Close() error {
	c.once.Do(func() { c.err = c.ReadCloser.Close() })
	return c.err
}

// ErrNoGetBody is a sentinel error returned when the connection cannot be reattempted
// due to GetBody not existing on the original request.
var ErrNoGetBody = errors.New("the GetBody function doesn't exist on the request")

func resetRequestBody(r *http.Request) error {
	if r.Body == nil || r.Body == http.NoBody {
		return nil
	}
	if r.GetBody == nil {
		return ErrNoGetBody
	}
	body, err := r.GetBody()
	if err != nil {
		return err
	}
	r.Body = body
	return nil
}
