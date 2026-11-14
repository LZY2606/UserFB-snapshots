// WebDriver BiDi client for object-oriented script evaluation.
// See https://www.w3.org/TR/webdriver-bidi/ for the protocol.
//
// The BiDi client is a side connection to an existing WebDriver session: it
// keeps script objects alive with "root" ownership so that they can be
// called repeatedly, unlike the HTTP ExecuteScript endpoint which only
// returns values.

package selenium

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"sync"

	"github.com/gorilla/websocket"
)

// Errors returned by the BiDi client.
var (
	// ErrBiDiUnavailable is returned by OpenBiDi when the WebDriver
	// implementation is not supported or the session did not negotiate a
	// usable webSocketUrl endpoint.
	ErrBiDiUnavailable = errors.New("selenium: BiDi endpoint unavailable")
	// ErrBiDiClosed is returned by BiDiClient methods after the connection
	// was closed, either by Close or by a transport failure.
	ErrBiDiClosed = errors.New("selenium: BiDi connection closed")
	// ErrBiDiNotObject is returned by EvaluateObject when the script result
	// has no handle and therefore cannot be kept as an object.
	ErrBiDiNotObject = errors.New("selenium: script result is not a BiDi object")
	// ErrBiDiObjectReleased is returned by Call after the object was
	// successfully released.
	ErrBiDiObjectReleased = errors.New("selenium: BiDi object already released")
	// ErrBiDiRealmDestroyed is returned when the realm that owns an object
	// was destroyed, e.g. by navigating away from the page.
	ErrBiDiRealmDestroyed = errors.New("selenium: BiDi realm destroyed")
)

// BiDiProtocolError is a top-level BiDi protocol error reply (type "error").
type BiDiProtocolError struct {
	Code       string
	Message    string
	Stacktrace string
}

// Error implements the error interface.
func (e *BiDiProtocolError) Error() string {
	return fmt.Sprintf("selenium: BiDi protocol error %s: %s", e.Code, e.Message)
}

// BiDiScriptError is a script exception reported inside a successful BiDi
// reply (result type "exception"). Details holds the raw exceptionDetails
// JSON.
type BiDiScriptError struct {
	Realm   string
	Details json.RawMessage
}

// Error implements the error interface.
func (e *BiDiScriptError) Error() string {
	return fmt.Sprintf("selenium: BiDi script exception in realm %s: %s", e.Realm, e.Details)
}

// bidiReply is the settlement of a single in-flight command.
type bidiReply struct {
	result json.RawMessage
	err    error
}

// bidiPending tracks one in-flight command. realm is the realm the command
// targets, or empty when the realm is not known yet (script.evaluate).
type bidiPending struct {
	realm string
	ch    chan bidiReply
}

// BiDiClient is a WebDriver BiDi side connection to an existing session.
type BiDiClient struct {
	conn *websocket.Conn

	mu        sync.Mutex
	nextID    int
	pending   map[int]*bidiPending
	destroyed map[string]bool
	closed    bool

	writeMu      sync.Mutex
	done         chan struct{}
	shutdownOnce sync.Once
}

// OpenBiDi opens a BiDi side connection for the session of wd. The endpoint
// negotiated during session creation (value.capabilities.webSocketUrl) is
// used; capabilities are not re-fetched. OpenBiDi subscribes to
// script.realmDestroyed and only returns after the subscription is
// confirmed. Only the *remoteWD created by NewRemote is supported.
func OpenBiDi(ctx context.Context, wd WebDriver) (*BiDiClient, error) {
	rwd, ok := wd.(*remoteWD)
	if !ok {
		return nil, ErrBiDiUnavailable
	}
	endpoint := rwd.bidiURL
	u, err := url.Parse(endpoint)
	if err != nil || (u.Scheme != "ws" && u.Scheme != "wss") {
		return nil, ErrBiDiUnavailable
	}

	conn, _, err := websocket.DefaultDialer.DialContext(ctx, endpoint, nil)
	if err != nil {
		return nil, err
	}
	c := &BiDiClient{
		conn:      conn,
		pending:   make(map[int]*bidiPending),
		destroyed: make(map[string]bool),
		done:      make(chan struct{}),
	}
	// Start receiving before subscribing: realm destruction events may
	// arrive before the subscription is confirmed and must not be lost.
	go c.recvLoop()
	if _, err := c.command(ctx, "session.subscribe", map[string]interface{}{
		"events": []string{"script.realmDestroyed"},
	}, ""); err != nil {
		c.Close()
		return nil, err
	}
	return c, nil
}

// Close shuts down the side connection and ends all pending waits. It does
// not end the WebDriver session. Close is idempotent.
func (c *BiDiClient) Close() error {
	c.shutdown()
	return nil
}

// shutdown terminates the connection and settles every pending command with
// ErrBiDiClosed. It is safe to call multiple times.
func (c *BiDiClient) shutdown() {
	c.shutdownOnce.Do(func() {
		c.mu.Lock()
		c.closed = true
		close(c.done)
		pending := c.pending
		c.pending = make(map[int]*bidiPending)
		c.mu.Unlock()
		for _, p := range pending {
			p.ch <- bidiReply{err: ErrBiDiClosed}
		}
		c.conn.Close()
	})
}

// command sends a BiDi command and waits for its reply. realm identifies the
// realm the command belongs to so that a script.realmDestroyed event can
// settle it; pass "" when the realm is not known in advance.
func (c *BiDiClient) command(ctx context.Context, method string, params interface{}, realm string) (json.RawMessage, error) {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return nil, ErrBiDiClosed
	}
	c.nextID++
	id := c.nextID
	p := &bidiPending{realm: realm, ch: make(chan bidiReply, 1)}
	c.pending[id] = p
	c.mu.Unlock()

	payload, err := json.Marshal(map[string]interface{}{
		"id":     id,
		"method": method,
		"params": params,
	})
	if err != nil {
		c.removePending(id)
		return nil, err
	}
	c.writeMu.Lock()
	err = c.conn.WriteMessage(websocket.TextMessage, payload)
	c.writeMu.Unlock()
	if err != nil {
		c.removePending(id)
		c.shutdown()
		return nil, ErrBiDiClosed
	}

	select {
	case r := <-p.ch:
		return r.result, r.err
	case <-ctx.Done():
		// The command is not replayed and its id is not reused; a late
		// reply finds no pending entry and is ignored.
		c.removePending(id)
		return nil, ctx.Err()
	case <-c.done:
		c.removePending(id)
		return nil, ErrBiDiClosed
	}
}

func (c *BiDiClient) removePending(id int) {
	c.mu.Lock()
	delete(c.pending, id)
	c.mu.Unlock()
}

// realmGone reports whether realm has been destroyed.
func (c *BiDiClient) realmGone(realm string) bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.destroyed[realm]
}

// recvLoop reads inbound messages until the transport fails or an invalid
// message arrives. It never waits on object locks or on other commands, so
// destruction events always reach in-flight calls.
func (c *BiDiClient) recvLoop() {
	for {
		_, data, err := c.conn.ReadMessage()
		if err != nil {
			c.shutdown()
			return
		}
		if !c.dispatch(data) {
			c.shutdown()
			return
		}
	}
}

// dispatch handles one inbound message. It returns false for invalid JSON,
// which terminates the connection.
func (c *BiDiClient) dispatch(data []byte) bool {
	var msg struct {
		ID         *int            `json:"id"`
		Type       string          `json:"type"`
		Method     string          `json:"method"`
		Params     json.RawMessage `json:"params"`
		Result     json.RawMessage `json:"result"`
		Error      string          `json:"error"`
		Message    string          `json:"message"`
		Stacktrace string          `json:"stacktrace"`
	}
	if err := json.Unmarshal(data, &msg); err != nil {
		return false
	}

	if msg.ID != nil {
		c.mu.Lock()
		p, ok := c.pending[*msg.ID]
		if ok {
			delete(c.pending, *msg.ID)
		}
		c.mu.Unlock()
		if !ok {
			// Late reply to a cancelled wait or unknown id: ignore.
			return true
		}
		if msg.Type == "error" {
			p.ch <- bidiReply{err: &BiDiProtocolError{
				Code:       msg.Error,
				Message:    msg.Message,
				Stacktrace: msg.Stacktrace,
			}}
		} else {
			p.ch <- bidiReply{result: msg.Result}
		}
		return true
	}

	if msg.Type == "event" && msg.Method == "script.realmDestroyed" {
		var params struct {
			Realm string `json:"realm"`
		}
		if err := json.Unmarshal(msg.Params, &params); err == nil && params.Realm != "" {
			c.realmDestroyed(params.Realm)
		}
	}
	// Unknown events are ignored.
	return true
}

// realmDestroyed records a destroyed realm and settles its in-flight
// commands, in inbound message order.
func (c *BiDiClient) realmDestroyed(realm string) {
	c.mu.Lock()
	c.destroyed[realm] = true
	var failed []*bidiPending
	for id, p := range c.pending {
		if p.realm == realm {
			delete(c.pending, id)
			failed = append(failed, p)
		}
	}
	c.mu.Unlock()
	for _, p := range failed {
		p.ch <- bidiReply{err: ErrBiDiRealmDestroyed}
	}
}

// bidiScriptResult is the parsed inner result of a script.evaluate or
// script.callFunction success reply.
type bidiScriptResult struct {
	realm  string
	result json.RawMessage
}

// parseScriptResult extracts the realm and the raw RemoteValue JSON from a
// script result. A result of type "exception" is returned as a
// *BiDiScriptError.
func parseScriptResult(raw json.RawMessage) (*bidiScriptResult, error) {
	var r struct {
		Type             string          `json:"type"`
		Realm            string          `json:"realm"`
		Result           json.RawMessage `json:"result"`
		ExceptionDetails json.RawMessage `json:"exceptionDetails"`
	}
	if err := json.Unmarshal(raw, &r); err != nil {
		return nil, err
	}
	if r.Type == "exception" {
		return nil, &BiDiScriptError{Realm: r.Realm, Details: r.ExceptionDetails}
	}
	return &bidiScriptResult{realm: r.Realm, result: r.Result}, nil
}

// EvaluateObject evaluates expression in the given browsing context and
// keeps the resulting object alive with root ownership. The result must
// carry a non-empty realm and a handle, otherwise ErrBiDiNotObject is
// returned.
func (c *BiDiClient) EvaluateObject(ctx context.Context, contextID, expression string) (*BiDiObject, error) {
	raw, err := c.command(ctx, "script.evaluate", map[string]interface{}{
		"expression":      expression,
		"target":          map[string]interface{}{"context": contextID},
		"awaitPromise":    true,
		"resultOwnership": "root",
	}, "")
	if err != nil {
		return nil, err
	}
	res, err := parseScriptResult(raw)
	if err != nil {
		return nil, err
	}
	var rv struct {
		Handle string `json:"handle"`
	}
	if err := json.Unmarshal(res.result, &rv); err != nil {
		return nil, err
	}
	if res.realm == "" || rv.Handle == "" {
		return nil, ErrBiDiNotObject
	}
	// The realm was unknown while the command was in flight; if it was
	// destroyed before this reply arrived, the object must not be
	// published.
	if c.realmGone(res.realm) {
		return nil, ErrBiDiRealmDestroyed
	}
	return &BiDiObject{client: c, realm: res.realm, handle: rv.Handle}, nil
}

// BiDiObject is a remote script object kept alive with root ownership in a
// specific realm.
type BiDiObject struct {
	client *BiDiClient
	realm  string
	handle string

	mu       sync.Mutex
	released bool
}

// Realm returns the realm the object belongs to.
func (o *BiDiObject) Realm() string {
	return o.realm
}

// Handle returns the BiDi handle of the object.
func (o *BiDiObject) Handle() string {
	return o.handle
}

// Call invokes declaration with the object as "this". args are BiDi
// LocalValue JSON documents, passed through unchanged. Calls on the same
// object are serialized with Release; different objects do not block each
// other. The return value is the raw RemoteValue JSON of the script result.
func (o *BiDiObject) Call(ctx context.Context, declaration string, args []json.RawMessage) (json.RawMessage, error) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.released {
		return nil, ErrBiDiObjectReleased
	}
	c := o.client
	if c.realmGone(o.realm) {
		return nil, ErrBiDiRealmDestroyed
	}
	if args == nil {
		args = []json.RawMessage{}
	}
	raw, err := c.command(ctx, "script.callFunction", map[string]interface{}{
		"functionDeclaration": declaration,
		"target":              map[string]interface{}{"realm": o.realm},
		"this":                map[string]interface{}{"handle": o.handle},
		"arguments":           args,
		"awaitPromise":        true,
		"resultOwnership":     "none",
	}, o.realm)
	if err != nil {
		return nil, err
	}
	res, err := parseScriptResult(raw)
	if err != nil {
		return nil, err
	}
	return res.result, nil
}

// Release disowns the remote object. A protocol rejection keeps the object
// usable and Release may be retried. After a successful Release, further
// calls return ErrBiDiObjectReleased and repeated Release calls return nil.
func (o *BiDiObject) Release(ctx context.Context) error {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.released {
		return nil
	}
	c := o.client
	if c.realmGone(o.realm) {
		return ErrBiDiRealmDestroyed
	}
	if _, err := c.command(ctx, "script.disown", map[string]interface{}{
		"target":  map[string]interface{}{"realm": o.realm},
		"handles": []string{o.handle},
	}, o.realm); err != nil {
		return err
	}
	o.released = true
	return nil
}
