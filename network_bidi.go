// WebDriver BiDi network interception support.
//
// OpenNetwork attaches to the WebSocket endpoint negotiated by NewRemote via
// the webSocketUrl capability and exposes the network interception primitives
// of the WebDriver BiDi protocol:
// https://www.w3.org/TR/webdriver-bidi/#module-network
//
// A typical use is intercepting a form submission to rewrite request headers
// before the request is sent, or to decide at response start whether to let
// it through. The plain HTTP WebDriver session is unaffected by this
// connection.

package selenium

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"

	"github.com/gorilla/websocket"
)

// Network intercept phases accepted by Network.Add.
const (
	// NetworkPhaseBeforeRequestSent pauses a request before it is sent.
	NetworkPhaseBeforeRequestSent = "beforeRequestSent"
	// NetworkPhaseResponseStarted pauses a request when the response
	// starts arriving.
	NetworkPhaseResponseStarted = "responseStarted"
)

// BiDiError is an error response returned by the remote end of a WebDriver
// BiDi connection. The protocol error code and message are preserved.
type BiDiError struct {
	// Err contains the protocol error code, e.g. "no such intercept".
	Err string `json:"error"`
	// Message is a human-readable description of the error.
	Message string `json:"message"`
}

// Error implements the error interface.
func (e *BiDiError) Error() string {
	return fmt.Sprintf("%s: %s", e.Err, e.Message)
}

// NetworkBytes is a string or base64-encoded binary value as used by the
// WebDriver BiDi protocol.
type NetworkBytes struct {
	// Type is "string" or "base64".
	Type string `json:"type"`
	// Value is the literal string or the base64-encoded data.
	Value string `json:"value"`
}

// NetworkHeader is a single HTTP header. Header order and duplicate names
// are significant and are preserved as given.
type NetworkHeader struct {
	Name  string       `json:"name"`
	Value NetworkBytes `json:"value"`
}

// errNetworkClosed reports use of a Network whose connection has ended.
var errNetworkClosed = errors.New("selenium: network connection is closed")

// errPauseSettled reports a decision attempted on a pause that is already
// completed, has a decision in flight, or was superseded by a later phase of
// its request.
var errPauseSettled = errors.New("selenium: network pause is no longer decidable")

// networkCommandResult is the outcome of one BiDi command.
type networkCommandResult struct {
	result json.RawMessage
	err    error
}

// Network is a WebDriver BiDi network interception session bound to the
// WebSocket endpoint negotiated by the WebDriver session. Closing it does
// not affect the HTTP WebDriver session.
type Network struct {
	conn *websocket.Conn

	// writeMu serializes writers on the WebSocket connection.
	writeMu sync.Mutex
	// cmdID generates command IDs unique per connection.
	cmdID uint64

	mu sync.Mutex
	// pending holds the reply channels of outstanding commands.
	pending map[uint64]chan networkCommandResult
	// queue holds received pauses not yet consumed by Next.
	queue []*NetworkPause
	// notify signals queue or connection state changes.
	notify chan struct{}
	// current maps a request ID to its most recent pause.
	current map[string]*NetworkPause
	closed  bool
	// closeErr is the error that ended the connection.
	closeErr error
}

// OpenNetwork opens the WebDriver BiDi network interception connection of
// wd, which must have been created by NewRemote with the webSocketUrl
// capability set to true. The negotiated endpoint is used as returned by the
// remote end; no capabilities are re-fetched.
//
// Only one network connection may be opened per driver, and only for the
// session that negotiated the WebSocket endpoint.
func OpenNetwork(wd WebDriver) (*Network, error) {
	rwd, ok := wd.(*remoteWD)
	if !ok {
		return nil, errors.New("selenium: OpenNetwork requires a WebDriver created by NewRemote")
	}

	rwd.networkMu.Lock()
	defer rwd.networkMu.Unlock()
	if rwd.networkOpened {
		return nil, errors.New("selenium: a network connection was already opened for this driver")
	}
	if rwd.webSocketURL == "" || rwd.id == "" || rwd.id != rwd.webSocketSession {
		return nil, errors.New("selenium: the session did not negotiate a webSocketUrl capability")
	}

	conn, _, err := websocket.DefaultDialer.Dial(rwd.webSocketURL, nil)
	if err != nil {
		return nil, fmt.Errorf("selenium: connecting to the negotiated webSocketUrl: %v", err)
	}
	n := newNetwork(conn)
	if _, err := n.sendCommand("session.subscribe", map[string]interface{}{
		"events": []string{"network.beforeRequestSent", "network.responseStarted"},
	}); err != nil {
		conn.Close()
		return nil, fmt.Errorf("selenium: subscribing to network events: %v", err)
	}

	rwd.networkOpened = true
	return n, nil
}

func newNetwork(conn *websocket.Conn) *Network {
	n := &Network{
		conn:    conn,
		pending: make(map[uint64]chan networkCommandResult),
		notify:  make(chan struct{}, 1),
		current: make(map[string]*NetworkPause),
	}
	go n.recvLoop()
	return n
}

// Add registers a network intercept rule. phases must be nonempty and
// contain only "beforeRequestSent" and/or "responseStarted". Each URL
// pattern is sent as a {"type":"string","pattern":s} urlPattern; a nil or
// empty patterns slice omits urlPatterns. The caller's slices are copied.
func (n *Network) Add(phases []string, patterns []string) (*NetworkIntercept, error) {
	if len(phases) == 0 {
		return nil, errors.New("selenium: network Add requires at least one phase")
	}
	pc := make([]string, len(phases))
	for i, phase := range phases {
		if phase != NetworkPhaseBeforeRequestSent && phase != NetworkPhaseResponseStarted {
			return nil, fmt.Errorf("selenium: invalid network intercept phase %q", phase)
		}
		pc[i] = phase
	}

	params := map[string]interface{}{"phases": pc}
	if len(patterns) > 0 {
		urlPatterns := make([]map[string]string, len(patterns))
		for i, pattern := range patterns {
			urlPatterns[i] = map[string]string{"type": "string", "pattern": pattern}
		}
		params["urlPatterns"] = urlPatterns
	}

	result, err := n.sendCommand("network.addIntercept", params)
	if err != nil {
		return nil, err
	}
	var parsed struct {
		Intercept string `json:"intercept"`
	}
	if err := json.Unmarshal(result, &parsed); err != nil {
		return nil, fmt.Errorf("selenium: decoding network.addIntercept result: %v", err)
	}
	return &NetworkIntercept{id: parsed.Intercept, n: n}, nil
}

// Next returns the next paused request in arrival order, blocking until one
// arrives, the connection ends, or ctx is done. Cancelling ctx cancels only
// this read.
func (n *Network) Next(ctx context.Context) (*NetworkPause, error) {
	for {
		if err := ctx.Err(); err != nil {
			return nil, err
		}
		n.mu.Lock()
		if len(n.queue) > 0 {
			p := n.queue[0]
			n.queue = n.queue[1:]
			n.mu.Unlock()
			return p, nil
		}
		if n.closed {
			err := n.closeErr
			n.mu.Unlock()
			return nil, err
		}
		notify := n.notify
		n.mu.Unlock()

		select {
		case <-notify:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
}

// Close ends the network connection and fails all pending operations. It
// does not affect the HTTP WebDriver session. Close is idempotent.
func (n *Network) Close() error {
	n.failAll(errNetworkClosed)
	return nil
}

// signalLocked wakes a blocked Next, if any. n.mu must be held.
func (n *Network) signalLocked() {
	select {
	case n.notify <- struct{}{}:
	default:
	}
}

// failAll ends the connection: all outstanding commands and blocked Next
// calls receive err, and later operations fail. It is idempotent.
func (n *Network) failAll(err error) {
	n.mu.Lock()
	if n.closed {
		n.mu.Unlock()
		return
	}
	n.closed = true
	n.closeErr = err
	pending := n.pending
	n.pending = make(map[uint64]chan networkCommandResult)
	n.signalLocked()
	n.mu.Unlock()

	for _, ch := range pending {
		ch <- networkCommandResult{err: err}
	}
	n.conn.Close()
}

// sendCommand sends one BiDi command and waits for its response. It is
// called from caller goroutines, never from the receive loop.
func (n *Network) sendCommand(method string, params interface{}) (json.RawMessage, error) {
	id := atomic.AddUint64(&n.cmdID, 1)
	ch := make(chan networkCommandResult, 1)

	n.mu.Lock()
	if n.closed {
		err := n.closeErr
		n.mu.Unlock()
		return nil, err
	}
	n.pending[id] = ch
	n.mu.Unlock()

	frame, err := json.Marshal(map[string]interface{}{
		"id":     id,
		"method": method,
		"params": params,
	})
	if err != nil {
		n.mu.Lock()
		delete(n.pending, id)
		n.mu.Unlock()
		return nil, err
	}

	n.writeMu.Lock()
	err = n.conn.WriteMessage(websocket.TextMessage, frame)
	n.writeMu.Unlock()
	if err != nil {
		// A transport failure ends the connection; the remote state is
		// unknown and nothing is replayed.
		n.failAll(fmt.Errorf("selenium: network write failed: %v", err))
	}

	res := <-ch
	return res.result, res.err
}

// bidiEnvelope is the shape of any frame received from the remote end.
type bidiEnvelope struct {
	Type    string          `json:"type"`
	ID      uint64          `json:"id"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params"`
	Result  json.RawMessage `json:"result"`
	Error   string          `json:"error"`
	Message string          `json:"message"`
}

// recvLoop dispatches incoming frames. It never waits for a command
// response itself.
func (n *Network) recvLoop() {
	for {
		_, data, err := n.conn.ReadMessage()
		if err != nil {
			n.failAll(fmt.Errorf("selenium: network connection lost: %v", err))
			return
		}
		var env bidiEnvelope
		if err := json.Unmarshal(data, &env); err != nil {
			continue
		}
		if env.Method != "" {
			n.handleEvent(env.Method, env.Params)
			continue
		}
		if env.ID == 0 {
			continue
		}
		n.mu.Lock()
		ch, ok := n.pending[env.ID]
		delete(n.pending, env.ID)
		n.mu.Unlock()
		if !ok {
			continue
		}
		res := networkCommandResult{result: env.Result}
		if env.Type == "error" || env.Error != "" {
			res.err = &BiDiError{Err: env.Error, Message: env.Message}
		}
		ch <- res
	}
}

// handleEvent folds one network event into the pause queue. n.mu is taken
// inside.
func (n *Network) handleEvent(method string, params json.RawMessage) {
	phase := strings.TrimPrefix(method, "network.")
	if phase != NetworkPhaseBeforeRequestSent && phase != NetworkPhaseResponseStarted {
		return
	}
	var parsed struct {
		Context       string   `json:"context"`
		IsBlocked     bool     `json:"isBlocked"`
		RedirectCount int      `json:"redirectCount"`
		Intercepts    []string `json:"intercepts"`
		Request       struct {
			Request string `json:"request"`
		} `json:"request"`
	}
	if err := json.Unmarshal(params, &parsed); err != nil {
		return
	}
	if !parsed.IsBlocked {
		return
	}

	// Copy everything handed to the caller.
	raw := append(json.RawMessage(nil), params...)
	intercepts := append([]string(nil), parsed.Intercepts...)

	n.mu.Lock()
	defer n.mu.Unlock()
	if n.closed {
		return
	}
	if cur := n.current[parsed.Request.Request]; cur != nil {
		if cur.RedirectCount == parsed.RedirectCount && cur.Phase == phase {
			// Duplicate event for an existing pause, possibly from
			// several matching rules: one pause per
			// (request, redirectCount, phase).
			return
		}
		// A new phase or redirect of the same request retires the
		// previous pause for future decisions.
		cur.retireLocked()
	}
	p := &NetworkPause{
		RequestID:     parsed.Request.Request,
		RedirectCount: parsed.RedirectCount,
		Phase:         phase,
		Context:       parsed.Context,
		Intercepts:    intercepts,
		Raw:           raw,
		network:       n,
	}
	n.current[p.RequestID] = p
	n.queue = append(n.queue, p)
	n.signalLocked()
}

// NetworkIntercept is a registered network intercept rule.
type NetworkIntercept struct {
	id string
	n  *Network

	mu sync.Mutex
	// removed is set once a removal was confirmed by the remote end.
	removed bool
	// inflight is the shared state of an outstanding removeIntercept
	// command, so concurrent Remove calls issue a single command.
	inflight *removeCall
}

// ID returns the intercept ID assigned by the remote end.
func (i *NetworkIntercept) ID() string {
	return i.id
}

// removeCall is the shared result of one outstanding removal.
type removeCall struct {
	done chan struct{}
	err  error
}

// Remove deregisters the rule. Rules removed this way only stop matching
// future requests; pauses already received stay decidable. A confirmed
// removal is idempotent. A confirmed error keeps the rule registered for a
// later explicit retry. Concurrent calls share one outstanding command.
func (i *NetworkIntercept) Remove() error {
	i.mu.Lock()
	if i.removed {
		i.mu.Unlock()
		return nil
	}
	if i.inflight != nil {
		call := i.inflight
		i.mu.Unlock()
		<-call.done
		return call.err
	}
	call := &removeCall{done: make(chan struct{})}
	i.inflight = call
	i.mu.Unlock()

	_, err := i.n.sendCommand("network.removeIntercept", map[string]interface{}{
		"intercept": i.id,
	})

	i.mu.Lock()
	if err == nil {
		i.removed = true
	}
	i.inflight = nil
	i.mu.Unlock()
	call.err = err
	close(call.done)
	return err
}

// Pause decision states.
const (
	pausePending = iota
	pauseInflight
	pauseCompleted
	pauseSuperseded
	pauseFailed
)

// NetworkPause is a request paused by a network intercept rule, handed to
// the caller for a decision: Continue or Fail.
type NetworkPause struct {
	// RequestID is the BiDi network request ID.
	RequestID string
	// RedirectCount is the number of redirects this request went through.
	RedirectCount int
	// Phase is "beforeRequestSent" or "responseStarted".
	Phase string
	// Context is the browsing context ID of the request.
	Context string
	// Intercepts lists the IDs of the rules that paused this request.
	Intercepts []string
	// Raw is the event params object, preserved byte-for-byte.
	Raw json.RawMessage

	network *Network
	// state and superseded are guarded by network.mu.
	state      int
	superseded bool
}

// retireLocked marks the pause as replaced by a newer phase or redirect of
// the same request. network.mu must be held.
func (p *NetworkPause) retireLocked() {
	p.superseded = true
	if p.state == pausePending {
		p.state = pauseSuperseded
	}
}

// Continue lets the request proceed. For the beforeRequestSent phase it
// sends network.continueRequest, for responseStarted it sends
// network.continueResponse. A nil headers slice leaves the headers
// untouched; a nonnil empty slice clears them. Header order, duplicate
// names and values are sent unchanged.
func (p *NetworkPause) Continue(headers []NetworkHeader) error {
	method := "network.continueRequest"
	if p.Phase == NetworkPhaseResponseStarted {
		method = "network.continueResponse"
	}
	params := map[string]interface{}{"request": p.RequestID}
	if headers != nil {
		params["headers"] = headers
	}
	return p.decide(method, params)
}

// Fail aborts the request with network.failRequest.
func (p *NetworkPause) Fail() error {
	return p.decide("network.failRequest", map[string]interface{}{
		"request": p.RequestID,
	})
}

// decide issues the pause's single decision command. A pause that is
// already decided, has a decision in flight, or was superseded by a newer
// phase of its request is rejected locally without sending a frame. A
// protocol error leaves the pause decidable for an explicit retry, unless a
// successor pause has since arrived. A transport error ends the connection.
func (p *NetworkPause) decide(method string, params map[string]interface{}) error {
	n := p.network

	n.mu.Lock()
	if p.state != pausePending {
		n.mu.Unlock()
		return errPauseSettled
	}
	if n.closed {
		err := n.closeErr
		n.mu.Unlock()
		return err
	}
	p.state = pauseInflight
	n.mu.Unlock()

	_, err := n.sendCommand(method, params)

	n.mu.Lock()
	defer n.mu.Unlock()
	if err != nil {
		if n.closed {
			// The connection is gone; the remote state is unknown and
			// nothing is replayed.
			p.state = pauseFailed
			return err
		}
		// A protocol rejection leaves the pause decidable only while it
		// is still the current pause of its request.
		if p.superseded {
			p.state = pauseSuperseded
		} else {
			p.state = pausePending
		}
		return err
	}
	// The reply completes exactly this pause, even if a successor event
	// arrived while it was in flight.
	p.state = pauseCompleted
	return nil
}
