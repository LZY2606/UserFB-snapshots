package graphql

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strconv"
	"sync"

	"github.com/gorilla/websocket"
)

// Errors reported by the subscription API.
var (
	// ErrSubscriptionsDisabled is returned when a subscription method is used
	// on a Client that was not created with WithSubscriptions.
	ErrSubscriptionsDisabled = errors.New("graphql: subscriptions are not configured")

	// ErrSubscriptionsClosed is returned when a subscription method is used
	// after CloseSubscriptions has been called.
	ErrSubscriptionsClosed = errors.New("graphql: subscriptions are closed")

	// ErrSlowConsumer is returned by Subscription.Next once all the results
	// accepted before a full result queue have been read. The subscription is
	// terminated and one complete message is sent to the server, but results
	// already accepted remain readable.
	ErrSlowConsumer = errors.New("graphql: subscription result queue is full")
)

// subscriptionProtocol is the WebSocket subprotocol spoken by subscriptions.
const subscriptionProtocol = "graphql-transport-ws"

// SubscriptionError is the terminal error of a subscription for which the
// server sent an error message. Payload holds the complete payload of that
// message, which is normally a JSON array of GraphQL errors.
type SubscriptionError struct {
	Payload json.RawMessage
}

func (e *SubscriptionError) Error() string {
	return "graphql: subscription error: " + string(e.Payload)
}

// Subscription is a single operation on the shared subscription connection.
// Create one with Client.Subscribe.
type Subscription struct {
	id       string
	capacity int
	manager  *subscriptionManager
	frame    []byte

	// lifetime is the context passed to Subscribe. It is stored so admission
	// can re-check cancellation at the moment the subscribe frame would be
	// written, closing the window before the watcher goroutine runs.
	lifetime context.Context

	notify chan struct{}
	done   chan struct{}

	mu               sync.Mutex
	queue            []json.RawMessage
	terminal         error
	sentSubscribe    bool
	serverTerminated bool
	completeSent     bool

	doneOnce sync.Once
}

// Next returns the payload of the next message for this subscription, in the
// order the messages were received. Payloads retain both their data and their
// errors and are not shared with any other result.
//
// The context only interrupts this call; it does not cancel the subscription.
// Once the subscription has terminated, Next drains the accepted results and
// then returns the terminal error: errors from the server as
// *SubscriptionError, a server complete as io.EOF, a full queue as
// ErrSlowConsumer, and cancellation as context.Canceled or the context's
// error.
func (s *Subscription) Next(ctx context.Context) (json.RawMessage, error) {
	if ctx == nil {
		return nil, errors.New("graphql: nil context")
	}
	for {
		s.mu.Lock()
		if len(s.queue) > 0 {
			payload := s.queue[0]
			s.queue[0] = nil
			s.queue = s.queue[1:]
			s.mu.Unlock()
			return payload, nil
		}
		if s.terminal != nil {
			err := s.terminal
			s.mu.Unlock()
			return nil, err
		}
		notify := s.notify
		s.mu.Unlock()

		select {
		case <-notify:
		case <-ctx.Done():
			return nil, ctx.Err()
		}
	}
}

// Cancel terminates the subscription. Results already accepted remain readable
// through Next. If the subscribe message for this operation was already sent,
// Cancel sends one complete message to the server, unless the server has
// already terminated the operation.
func (s *Subscription) Cancel() {
	s.finishLocal(context.Canceled)
}

func (s *Subscription) signal() {
	select {
	case s.notify <- struct{}{}:
	default:
	}
}

func (s *Subscription) stopWatcher() {
	s.doneOnce.Do(func() { close(s.done) })
}

func (s *Subscription) watch(ctx context.Context) {
	go func() {
		select {
		case <-ctx.Done():
			s.finishLocal(ctx.Err())
		case <-s.done:
		}
	}()
}

// finishLocal terminates the subscription because of local cancellation. The
// server is told to stop the operation at most once, and only if it was
// already asked to start it.
func (s *Subscription) finishLocal(err error) {
	s.mu.Lock()
	if s.terminal != nil {
		s.mu.Unlock()
		return
	}
	s.terminal = err
	if s.sentSubscribe && !s.serverTerminated && !s.completeSent {
		s.completeSent = true
		s.manager.enqueue(completeFrame(s.id))
	}
	s.mu.Unlock()
	s.manager.forget(s.id)

	s.signal()
	s.stopWatcher()
}

// finishServer terminates the subscription on behalf of the server or because
// the connection ended. No complete message is sent back.
func (s *Subscription) finishServer(err error) {
	s.mu.Lock()
	if s.terminal != nil {
		s.mu.Unlock()
		return
	}
	s.terminal = err
	s.serverTerminated = true
	s.mu.Unlock()
	s.manager.forget(s.id)

	s.signal()
	s.stopWatcher()
}

// deliver queues a next payload. It never blocks: a subscription that cannot
// keep up is terminated on its own, leaving the shared read loop and the other
// subscriptions untouched.
func (s *Subscription) deliver(payload []byte) {
	s.mu.Lock()
	if s.terminal != nil {
		s.mu.Unlock()
		return
	}
	if len(s.queue) >= s.capacity {
		s.terminal = ErrSlowConsumer
		s.serverTerminated = true
		if s.sentSubscribe && !s.completeSent {
			s.completeSent = true
			s.manager.enqueue(completeFrame(s.id))
		}
		s.mu.Unlock()
		s.manager.forget(s.id)
		s.signal()
		s.stopWatcher()
		return
	}
	s.queue = append(s.queue, json.RawMessage(append([]byte(nil), payload...)))
	s.mu.Unlock()
	s.signal()
}

// subscriptionClientState is the subscription configuration and connection
// state owned by a Client. All fields are guarded by mu.
type subscriptionClientState struct {
	configured bool
	url        string
	header     http.Header

	mu      sync.Mutex
	manager *subscriptionManager
	closed  bool
	nextID  uint64

	// netDial is a test hook. When nil the standard Gorilla dialer is used.
	netDial func(ctx context.Context, network, addr string) (net.Conn, error)
}

type wsMessage struct {
	ID      string          `json:"id"`
	Type    string          `json:"type"`
	Payload json.RawMessage `json:"payload"`
}

type subscriptionManager struct {
	conn *websocket.Conn

	mu      sync.Mutex
	ops     map[string]*Subscription
	pending []*Subscription
	acked   bool
	closed  bool
	failErr error

	outMu     sync.Mutex
	outCond   *sync.Cond
	out       [][]byte
	outClosed bool

	readDone  chan struct{}
	writeDone chan struct{}
}

func newSubscriptionManager(conn *websocket.Conn) *subscriptionManager {
	m := &subscriptionManager{
		conn:      conn,
		ops:       make(map[string]*Subscription),
		readDone:  make(chan struct{}),
		writeDone: make(chan struct{}),
	}
	m.outCond = sync.NewCond(&m.outMu)
	return m
}

// WithSubscriptions configures the WebSocket endpoint used by Subscribe. The
// header is used only for the WebSocket handshake; it is never derived from
// Request.Header or from the HTTP client.
func WithSubscriptions(wsURL string, header http.Header) ClientOption {
	return func(client *Client) {
		client.subs.configured = true
		client.subs.url = wsURL
		client.subs.header = header.Clone()
	}
}

// Subscribe starts a GraphQL subscription. Query and variables are snapshotted
// from req at the time of the call. The returned Subscription does not wait
// for the server's connection acknowledgement; the subscribe message is sent
// once the connection is acknowledged.
//
// The context is the lifetime of this subscription. Cancelling it does not
// close the shared connection.
func (c *Client) Subscribe(ctx context.Context, req *Request, capacity int) (*Subscription, error) {
	if !c.subs.configured {
		return nil, ErrSubscriptionsDisabled
	}
	if ctx == nil {
		return nil, errors.New("graphql: nil context")
	}
	if req == nil {
		return nil, errors.New("graphql: nil request")
	}
	if len(req.files) > 0 {
		return nil, errors.New("graphql: subscriptions do not support files")
	}
	if len(req.Header) > 0 {
		return nil, errors.New("graphql: subscriptions do not support Request.Header")
	}
	if capacity <= 0 {
		return nil, fmt.Errorf("graphql: subscription capacity must be positive, got %d", capacity)
	}
	if err := ctx.Err(); err != nil {
		return nil, err
	}

	// Encode the operation now so later mutation of req cannot affect the
	// message that is eventually sent.
	payload, err := json.Marshal(struct {
		Query     string                 `json:"query"`
		Variables map[string]interface{} `json:"variables"`
	}{
		Query:     req.q,
		Variables: req.vars,
	})
	if err != nil {
		return nil, fmt.Errorf("graphql: encode subscription payload: %w", err)
	}

	c.subs.mu.Lock()
	defer c.subs.mu.Unlock()
	if c.subs.closed {
		return nil, ErrSubscriptionsClosed
	}
	m := c.subs.manager
	if m == nil {
		conn, err := c.dialSubscriptions(ctx)
		if err != nil {
			return nil, err
		}
		m = newSubscriptionManager(conn)
		c.subs.manager = m
		go m.readLoop()
		go m.writeLoop()
		m.enqueue([]byte(`{"type":"connection_init","payload":{}}`))
	}

	c.subs.nextID++
	id := strconv.FormatUint(c.subs.nextID, 10)
	sub := &Subscription{
		id:       id,
		capacity: capacity,
		manager:  m,
		frame:    subscribeFrame(id, payload),
		lifetime: ctx,
		notify:   make(chan struct{}, 1),
		done:     make(chan struct{}),
	}
	if err := m.register(sub); err != nil {
		return nil, err
	}
	sub.watch(ctx)
	return sub, nil
}

// CloseSubscriptions closes the shared subscription connection, terminates all
// active subscriptions and waits for the connection's read and write
// goroutines to exit. It is idempotent and does not wait for consumers to
// drain their result queues. Later calls to Subscribe return
// ErrSubscriptionsClosed.
func (c *Client) CloseSubscriptions() error {
	if !c.subs.configured {
		return ErrSubscriptionsDisabled
	}
	c.subs.mu.Lock()
	if c.subs.closed {
		c.subs.mu.Unlock()
		return nil
	}
	c.subs.closed = true
	m := c.subs.manager
	c.subs.mu.Unlock()

	if m == nil {
		return nil
	}
	m.shutdown(ErrSubscriptionsClosed)
	<-m.readDone
	<-m.writeDone
	return nil
}

func (c *Client) dialSubscriptions(ctx context.Context) (*websocket.Conn, error) {
	dialer := websocket.Dialer{Subprotocols: []string{subscriptionProtocol}}
	if c.subs.netDial != nil {
		dialer.NetDialContext = c.subs.netDial
	}
	conn, resp, err := dialer.DialContext(ctx, c.subs.url, c.subs.header.Clone())
	if resp != nil && resp.Body != nil {
		resp.Body.Close()
	}
	if err != nil {
		return nil, fmt.Errorf("graphql: dial subscriptions: %w", err)
	}
	return conn, nil
}

func (m *subscriptionManager) terminalErrLocked() error {
	if m.failErr != nil {
		return m.failErr
	}
	return ErrSubscriptionsClosed
}

func (m *subscriptionManager) register(s *Subscription) error {
	m.mu.Lock()
	if m.closed {
		err := m.terminalErrLocked()
		m.mu.Unlock()
		return err
	}
	m.ops[s.id] = s
	if m.acked {
		m.admitLocked(s)
	} else {
		m.pending = append(m.pending, s)
	}
	m.mu.Unlock()
	return nil
}

// admitLocked sends the subscribe frame for an operation that may be released.
// The lifetime context is checked again here: an operation cancelled while it
// waited for the acknowledgement must never reach the wire. m.mu must be held.
func (m *subscriptionManager) admitLocked(s *Subscription) {
	s.mu.Lock()
	if s.terminal != nil {
		s.mu.Unlock()
		return
	}
	if err := s.lifetime.Err(); err != nil {
		s.terminal = err
		s.serverTerminated = true
		delete(m.ops, s.id)
		s.mu.Unlock()
		s.signal()
		s.stopWatcher()
		return
	}
	s.sentSubscribe = true
	m.enqueue(s.frame)
	s.mu.Unlock()
}

func (m *subscriptionManager) lookup(id string) *Subscription {
	m.mu.Lock()
	s := m.ops[id]
	m.mu.Unlock()
	return s
}

// lookupAdmitted returns the operation for id only if it has been admitted and
// is still active. Messages for operations that are waiting for the
// acknowledgement or have already finished are ignored.
func (m *subscriptionManager) lookupAdmitted(id string) *Subscription {
	s := m.lookup(id)
	if s == nil {
		return nil
	}
	s.mu.Lock()
	admitted := s.sentSubscribe && s.terminal == nil
	s.mu.Unlock()
	if !admitted {
		return nil
	}
	return s
}

// forget drops a finished operation so late frames for its id are ignored.
func (m *subscriptionManager) forget(id string) {
	m.mu.Lock()
	delete(m.ops, id)
	m.mu.Unlock()
}

func (m *subscriptionManager) enqueue(frame []byte) {
	m.outMu.Lock()
	if !m.outClosed {
		m.out = append(m.out, frame)
		m.outCond.Signal()
	}
	m.outMu.Unlock()
}

func (m *subscriptionManager) stopWriter() {
	m.outMu.Lock()
	m.outClosed = true
	m.outCond.Broadcast()
	m.outMu.Unlock()
}

func (m *subscriptionManager) writeLoop() {
	defer close(m.writeDone)
	for {
		m.outMu.Lock()
		for len(m.out) == 0 && !m.outClosed {
			m.outCond.Wait()
		}
		if len(m.out) == 0 && m.outClosed {
			m.outMu.Unlock()
			return
		}
		frame := m.out[0]
		m.out[0] = nil
		m.out = m.out[1:]
		m.outMu.Unlock()

		if err := m.conn.WriteMessage(websocket.TextMessage, frame); err != nil {
			m.shutdown(fmt.Errorf("graphql: subscription connection lost: %w", err))
			return
		}
	}
}

func (m *subscriptionManager) readLoop() {
	defer close(m.readDone)
	for {
		_, data, err := m.conn.ReadMessage()
		if err != nil {
			m.shutdown(fmt.Errorf("graphql: subscription connection lost: %w", err))
			return
		}
		if err := m.handleMessage(data); err != nil {
			m.shutdown(err)
			return
		}
	}
}

func (m *subscriptionManager) handleMessage(data []byte) error {
	var msg wsMessage
	if err := json.Unmarshal(data, &msg); err != nil {
		return fmt.Errorf("graphql: malformed subscription message: %w", err)
	}
	switch msg.Type {
	case "connection_ack":
		m.ack()
	case "ping":
		m.enqueue(pongFrame(msg.Payload))
	case "pong":
		// Unsolicited pongs carry no operation data.
	case "next":
		if msg.ID == "" {
			return nil
		}
		if s := m.lookupAdmitted(msg.ID); s != nil {
			s.deliver(msg.Payload)
		}
	case "error":
		if msg.ID == "" {
			// An error with no operation id is a connection-level error: it
			// terminates every active operation, just like a broken connection.
			return &SubscriptionError{Payload: json.RawMessage(append([]byte(nil), msg.Payload...))}
		}
		if s := m.lookup(msg.ID); s != nil {
			s.finishServer(&SubscriptionError{Payload: json.RawMessage(append([]byte(nil), msg.Payload...))})
		}
	case "complete":
		if msg.ID == "" {
			return nil
		}
		if s := m.lookup(msg.ID); s != nil {
			s.finishServer(io.EOF)
		}
	default:
		return fmt.Errorf("graphql: unknown subscription message type %q", msg.Type)
	}
	return nil
}

// ack releases the operations admitted before the acknowledgement, in
// admission order. It is safe against concurrent Subscribe calls because both
// run under m.mu.
func (m *subscriptionManager) ack() {
	m.mu.Lock()
	if m.closed || m.acked {
		m.mu.Unlock()
		return
	}
	m.acked = true
	pending := m.pending
	m.pending = nil
	for _, s := range pending {
		m.admitLocked(s)
	}
	m.mu.Unlock()
}

// shutdown terminates every operation and closes the socket. It may be called
// from any goroutine; only the first call has an effect.
func (m *subscriptionManager) shutdown(reason error) {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	m.closed = true
	m.failErr = reason
	// Every registered operation, whether already admitted or still waiting
	// for the acknowledgement, lives in m.ops.
	ops := make([]*Subscription, 0, len(m.ops))
	for _, s := range m.ops {
		ops = append(ops, s)
	}
	m.pending = nil
	m.mu.Unlock()

	for _, s := range ops {
		s.finishServer(reason)
	}
	m.stopWriter()
	m.conn.Close()
}

func subscribeFrame(id string, payload json.RawMessage) []byte {
	frame := make([]byte, 0, len(payload)+len(id)+48)
	frame = append(frame, `{"id":`...)
	frame = append(frame, strconv.Quote(id)...)
	frame = append(frame, `,"type":"subscribe","payload":`...)
	frame = append(frame, payload...)
	frame = append(frame, '}')
	return frame
}

func completeFrame(id string) []byte {
	frame := make([]byte, 0, len(id)+32)
	frame = append(frame, `{"id":`...)
	frame = append(frame, strconv.Quote(id)...)
	frame = append(frame, `,"type":"complete"}`...)
	return frame
}

func pongFrame(payload json.RawMessage) []byte {
	if payload == nil {
		return []byte(`{"type":"pong"}`)
	}
	frame := make([]byte, 0, len(payload)+24)
	frame = append(frame, `{"type":"pong","payload":`...)
	frame = append(frame, payload...)
	frame = append(frame, '}')
	return frame
}
