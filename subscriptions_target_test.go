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
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/machinebox/graphql/task_support/taskcheck"
)

const subscriptionTestTimeout = 5 * time.Second

// pipeAddr is the fake address reported by pipeListener.
type pipeAddr struct{}

func (pipeAddr) Network() string { return "pipe" }
func (pipeAddr) String() string  { return "subscriptions.test" }

// pipeListener serves httptest connections over net.Pipe so the tests do not
// depend on local port binding. The server side of each pipe is handed to
// Accept and the client side is handed to the client's dial hook.
type pipeListener struct {
	conns chan net.Conn
	done  chan struct{}
	once  sync.Once
}

func newPipeListener() *pipeListener {
	return &pipeListener{conns: make(chan net.Conn), done: make(chan struct{})}
}

func (l *pipeListener) Accept() (net.Conn, error) {
	select {
	case c := <-l.conns:
		return c, nil
	case <-l.done:
		return nil, errors.New("pipe listener closed")
	}
}

func (l *pipeListener) Close() error {
	l.once.Do(func() { close(l.done) })
	return nil
}

func (l *pipeListener) Addr() net.Addr { return pipeAddr{} }

// Dial is the client side of the listener. It pairs a fresh pipe with Accept.
func (l *pipeListener) Dial(ctx context.Context, network, addr string) (net.Conn, error) {
	client, server := net.Pipe()
	select {
	case l.conns <- server:
		return client, nil
	case <-l.done:
		client.Close()
		server.Close()
		return nil, errors.New("pipe listener closed")
	case <-ctx.Done():
		client.Close()
		server.Close()
		return nil, ctx.Err()
	}
}

type wsFrame struct {
	ID      string          `json:"id"`
	Type    string          `json:"type"`
	Payload json.RawMessage `json:"payload"`
}

// subServer is a Gorilla WebSocket server whose frames are read into an
// unbuffered channel, so the test controls when each frame is consumed.
type subServer struct {
	t        *testing.T
	srv      *http.Server
	listener *pipeListener

	upgrades atomic.Int32

	subprotocolMu sync.Mutex
	subprotocol   string
	handshake     http.Header

	connCh   chan *websocket.Conn
	frames   chan wsFrame
	readErrs chan error
	done     chan struct{}
	closeOne sync.Once

	connMu sync.Mutex
	conn   *websocket.Conn
}

func newSubServer(t *testing.T) *subServer {
	t.Helper()
	s := &subServer{
		t:        t,
		listener: newPipeListener(),
		connCh:   make(chan *websocket.Conn),
		frames:   make(chan wsFrame),
		readErrs: make(chan error, 1),
		done:     make(chan struct{}),
	}
	upgrader := websocket.Upgrader{
		Subprotocols: []string{subscriptionProtocol},
		CheckOrigin:  func(*http.Request) bool { return true },
	}
	s.srv = &http.Server{Handler: http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.subprotocolMu.Lock()
		s.subprotocol = r.Header.Get("Sec-WebSocket-Protocol")
		s.handshake = r.Header.Clone()
		s.subprotocolMu.Unlock()
		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		s.upgrades.Add(1)
		select {
		case s.connCh <- conn:
		case <-s.done:
			conn.Close()
			return
		}
		defer conn.Close()
		for {
			_, data, err := conn.ReadMessage()
			if err != nil {
				select {
				case s.readErrs <- err:
				default:
				}
				return
			}
			var f wsFrame
			if err := json.Unmarshal(data, &f); err != nil {
				select {
				case s.readErrs <- fmt.Errorf("frame decode: %w", err):
				default:
				}
				return
			}
			select {
			case s.frames <- f:
			case <-s.done:
				return
			}
		}
	})}
	go s.srv.Serve(s.listener)
	t.Cleanup(s.Close)
	return s
}

func (s *subServer) Close() {
	s.closeOne.Do(func() {
		close(s.done)
		s.connMu.Lock()
		if s.conn != nil {
			s.conn.Close()
		}
		s.connMu.Unlock()
		s.listener.Close()
		closeCtx, cancel := context.WithTimeout(context.Background(), subscriptionTestTimeout)
		defer cancel()
		s.srv.Shutdown(closeCtx)
	})
}

// connFor waits for the upgraded server-side connection and records it.
func (s *subServer) connFor() *websocket.Conn {
	s.t.Helper()
	select {
	case c := <-s.connCh:
		s.connMu.Lock()
		s.conn = c
		s.connMu.Unlock()
		return c
	case <-time.After(subscriptionTestTimeout):
		s.t.Fatal("timed out waiting for the subscription connection")
		return nil
	}
}

// recv returns the next frame the server read from the client.
func (s *subServer) recv() wsFrame {
	s.t.Helper()
	select {
	case f := <-s.frames:
		return f
	case <-time.After(subscriptionTestTimeout):
		s.t.Fatal("timed out waiting for a client frame")
		return wsFrame{}
	}
}

// barrier sends a ping and returns every frame the client wrote before the
// matching pong. Because the client's writer is ordered, this determines
// exactly which frames were produced before the ping was processed, without
// relying on timing.
func (s *subServer) barrier() []wsFrame {
	s.t.Helper()
	s.writeRaw([]byte(`{"type":"ping","payload":{}}`))
	var frames []wsFrame
	for {
		f := s.recv()
		if f.Type == "pong" {
			return frames
		}
		frames = append(frames, f)
	}
}

func (s *subServer) writeRaw(data []byte) {
	s.t.Helper()
	s.connMu.Lock()
	defer s.connMu.Unlock()
	if s.conn == nil {
		s.t.Fatal("server has no connection")
	}
	if err := s.conn.WriteMessage(websocket.TextMessage, data); err != nil {
		s.t.Fatalf("server write: %v", err)
	}
}

func (s *subServer) send(typ, id string, payload json.RawMessage) {
	s.t.Helper()
	frame := struct {
		ID      string          `json:"id,omitempty"`
		Type    string          `json:"type"`
		Payload json.RawMessage `json:"payload,omitempty"`
	}{ID: id, Type: typ, Payload: payload}
	data, err := json.Marshal(frame)
	if err != nil {
		s.t.Fatalf("encode server frame: %v", err)
	}
	s.writeRaw(data)
}

func (s *subServer) sendNext(id, payload string) {
	s.t.Helper()
	s.send("next", id, json.RawMessage(payload))
}

// subscriptionClient returns a client wired to s. The HTTP endpoint is unused;
// only the WebSocket URL is dialed and the handshake header comes from
// WithSubscriptions.
func subscriptionClient(t *testing.T, s *subServer) *Client {
	t.Helper()
	header := http.Header{}
	header.Set("X-Subscription-Handshake", "configured")
	c := NewClient("http://http.invalid/graphql", WithSubscriptions("ws://subscriptions.invalid/graphql", header))
	c.subs.netDial = s.listener.Dial
	return c
}

func checkFrame(c *taskcheck.C, f wsFrame, typ, id string) {
	c.Check(f.Type == typ, "frame type = %q, want %q", f.Type, typ)
	if id != "" {
		c.Check(f.ID == id, "frame id = %q, want %q", f.ID, id)
	}
}

// nextWithin reads one result with a bounded wait and reports the payload.
func nextWithin(t *testing.T, sub *Subscription) (json.RawMessage, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), subscriptionTestTimeout)
	defer cancel()
	return sub.Next(ctx)
}

func TestSubscriptionAdmission(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("Disabled", func(t *testing.T) {
			client := NewClient("http://http.invalid/graphql")
			sub, err := client.Subscribe(context.Background(), NewRequest("subscription { n }"), 1)
			c.Check(sub == nil, "Subscribe on an unconfigured client returned a subscription")
			c.Check(errors.Is(err, ErrSubscriptionsDisabled), "error = %v, want ErrSubscriptionsDisabled", err)
			c.Check(errors.Is(client.CloseSubscriptions(), ErrSubscriptionsDisabled),
				"CloseSubscriptions on an unconfigured client did not report ErrSubscriptionsDisabled")
		})

		t.Run("LocalInputErrors", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)

			withFiles := NewRequest("subscription { n }")
			withFiles.File("a", "a.txt", strings.NewReader("data"))
			_, err := client.Subscribe(context.Background(), withFiles, 1)
			c.Check(err != nil && strings.Contains(err.Error(), "files"), "file request error = %v", err)

			withHeader := NewRequest("subscription { n }")
			withHeader.Header.Set("X-Request", "1")
			_, err = client.Subscribe(context.Background(), withHeader, 1)
			c.Check(err != nil && strings.Contains(err.Error(), "Header"), "header request error = %v", err)

			for _, capacity := range []int{0, -3} {
				_, err = client.Subscribe(context.Background(), NewRequest("subscription { n }"), capacity)
				c.Check(err != nil && strings.Contains(err.Error(), "capacity"), "capacity %d error = %v", capacity, err)
			}

			_, err = client.Subscribe(context.Background(), nil, 1)
			c.Check(err != nil, "nil request error = %v", err)

			// None of the rejected calls may have opened a connection.
			c.Check(s.upgrades.Load() == 0, "rejected inputs opened %d connections", s.upgrades.Load())
		})

		t.Run("Snapshot", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)

			req := NewRequest("subscription($k:Int!){ tick(k:$k) }")
			req.Var("k", 1)
			sub, err := client.Subscribe(context.Background(), req, 2)
			c.Check(err == nil, "Subscribe error = %v", err)
			if err != nil {
				return
			}
			_ = s.connFor()

			init := s.recv()
			checkFrame(c, init, "connection_init", "")
			c.Check(string(init.Payload) == "{}", "connection_init payload = %s, want {}", init.Payload)
			s.subprotocolMu.Lock()
			proto, handshake := s.subprotocol, s.handshake.Clone()
			s.subprotocolMu.Unlock()
			c.Check(proto == subscriptionProtocol, "subprotocol = %q, want %q", proto, subscriptionProtocol)
			c.Check(handshake.Get("X-Subscription-Handshake") == "configured",
				"handshake header = %q, want configured", handshake.Get("X-Subscription-Handshake"))
			c.Check(handshake.Get("X-Request") == "", "Request.Header leaked into the handshake")

			// Mutate the request after Subscribe: the wire payload must keep
			// the values captured at call time.
			req.Var("k", 99)
			req.q = "subscription { other }"

			s.send("connection_ack", "", nil)
			f := s.recv()
			checkFrame(c, f, "subscribe", "")
			c.Check(f.ID != "", "subscribe id is empty")
			var payload struct {
				Query     string                 `json:"query"`
				Variables map[string]interface{} `json:"variables"`
			}
			decErr := json.Unmarshal(f.Payload, &payload)
			c.Check(decErr == nil, "subscribe payload decode: %v", decErr)
			c.Check(payload.Query == "subscription($k:Int!){ tick(k:$k) }", "snapshot query = %q", payload.Query)
			key, _ := payload.Variables["k"].(float64)
			c.Check(key == 1, "snapshot variable k = %v, want 1", payload.Variables["k"])

			sub.Cancel()
			checkFrame(c, s.recv(), "complete", f.ID)
			c.Check(client.CloseSubscriptions() == nil, "CloseSubscriptions error")
		})

		t.Run("ConcurrentSetup", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)

			const callers = 8
			start := make(chan struct{})
			var wg sync.WaitGroup
			subs := make(chan *Subscription, callers)
			errs := make(chan error, callers)
			for i := 0; i < callers; i++ {
				wg.Add(1)
				go func() {
					defer wg.Done()
					<-start
					sub, err := client.Subscribe(context.Background(), NewRequest("subscription { n }"), 4)
					if err != nil {
						errs <- err
						return
					}
					subs <- sub
				}()
			}
			close(start)
			wg.Wait()
			close(subs)
			close(errs)
			for err := range errs {
				c.Check(false, "concurrent Subscribe error = %v", err)
			}
			_ = s.connFor()

			init := s.recv()
			checkFrame(c, init, "connection_init", "")
			early := s.barrier()
			c.Check(len(early) == 0, "frames sent before the ack: %+v", early)
			c.Check(s.upgrades.Load() == 1, "connections = %d, want exactly 1", s.upgrades.Load())

			s.send("connection_ack", "", nil)
			ids := map[string]bool{}
			subByID := map[string]*Subscription{}
			prevID := ""
			for i := 0; i < callers; i++ {
				f := s.recv()
				checkFrame(c, f, "subscribe", "")
				c.Check(f.ID != "", "concurrent subscribe id is empty")
				c.Check(!ids[f.ID], "duplicate subscription id %q", f.ID)
				// Admission order is the order the ids were assigned.
				if prevID != "" {
					prev, _ := strconv.Atoi(prevID)
					cur, _ := strconv.Atoi(f.ID)
					c.Check(cur > prev, "subscribe frames out of admission order: %q then %q", prevID, f.ID)
				}
				prevID = f.ID
				ids[f.ID] = true
			}
			for sub := range subs {
				subByID[sub.id] = sub
			}
			c.Check(len(subByID) == callers, "distinct subscriptions = %d, want %d", len(subByID), callers)

			// Cancel exactly one operation: precisely one complete must follow,
			// carrying that operation's id.
			var stopped *Subscription
			for _, sub := range subByID {
				stopped = sub
				break
			}
			stopped.Cancel()
			checkFrame(c, s.recv(), "complete", stopped.id)
			extra := s.barrier()
			c.Check(len(extra) == 0, "unexpected frames after a single Cancel: %+v", extra)
			c.Check(client.CloseSubscriptions() == nil, "CloseSubscriptions error")
		})
	})
}

// ackedPair starts two subscriptions on a fresh server and acknowledges the
// connection. It returns the client, both subscriptions mapped by their wire
// ids and the frame ids in admission order.
func ackedPair(t *testing.T, capacity int) (*subServer, *Client, *Subscription, *Subscription, []string) {
	t.Helper()
	s := newSubServer(t)
	client := subscriptionClient(t, s)
	first := NewRequest("subscription { a }")
	first.Var("a", 1)
	second := NewRequest("subscription { b }")
	second.Var("b", 2)
	sub1, err := client.Subscribe(context.Background(), first, capacity)
	if err != nil {
		t.Fatalf("Subscribe first: %v", err)
	}
	sub2, err := client.Subscribe(context.Background(), second, capacity)
	if err != nil {
		t.Fatalf("Subscribe second: %v", err)
	}
	s.connFor()
	if f := s.recv(); f.Type != "connection_init" {
		t.Fatalf("first frame type = %q, want connection_init", f.Type)
	}
	s.send("connection_ack", "", nil)
	f1 := s.recv()
	f2 := s.recv()
	if f1.Type != "subscribe" || f2.Type != "subscribe" {
		t.Fatalf("after ack frames = %q/%q, want subscribe/subscribe", f1.Type, f2.Type)
	}
	if f1.ID != sub1.id || f2.ID != sub2.id {
		t.Fatalf("wire ids %q/%q do not match handles %q/%q", f1.ID, f2.ID, sub1.id, sub2.id)
	}
	return s, client, sub1, sub2, []string{f1.ID, f2.ID}
}

func TestSubscriptionDispatch(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("OrderAndPayloads", func(t *testing.T) {
			s, client, sub1, sub2, ids := ackedPair(t, 8)
			defer client.CloseSubscriptions()

			p1 := `{"data":{"a":{"v":1}},"errors":[{"message":"soft"}]}`
			s.sendNext(ids[0], p1)
			s.sendNext(ids[1], `{"data":{"b":2}}`)
			s.sendNext(ids[0], `{"data":{"a":2},"extensions":{"n":3}}`)

			got, err := nextWithin(t, sub1)
			c.Check(err == nil, "first next error = %v", err)
			c.Check(string(got) == p1, "first payload = %s, want %s", got, p1)
			first := append(json.RawMessage(nil), got...)

			got2, err := nextWithin(t, sub2)
			c.Check(err == nil, "second subscription next error = %v", err)
			c.Check(string(got2) == `{"data":{"b":2}}`, "second subscription payload = %s", got2)

			got, err = nextWithin(t, sub1)
			c.Check(err == nil, "third next error = %v", err)
			c.Check(string(got) == `{"data":{"a":2},"extensions":{"n":3}}`, "third payload = %s", got)

			// Result bytes must not be shared between results: mutating one
			// returned payload leaves the other intact.
			c.Check(&first[0] != &got2[0], "result buffers are shared")
			first[0] = 'X'
			c.Check(string(got2) == `{"data":{"b":2}}`, "mutating one result changed another: %s", got2)
			c.Check(string(first) == "X"+p1[1:], "the mutated result changed unexpectedly: %s", first)
		})

		t.Run("PingPong", func(t *testing.T) {
			s, client, sub1, _, ids := ackedPair(t, 2)
			defer client.CloseSubscriptions()

			pingPayload := `{"nonce":"abc"}`
			s.writeRaw([]byte(`{"type":"ping","payload":{"nonce":"abc"}}`))
			pong := s.recv()
			checkFrame(c, pong, "pong", "")
			c.Check(string(pong.Payload) == pingPayload, "pong payload = %s, want %s", pong.Payload, pingPayload)

			// The ping must not have been delivered to the subscription.
			s.sendNext(ids[0], `{"data":{"a":"after-ping"}}`)
			got, err := nextWithin(t, sub1)
			c.Check(err == nil, "next after ping error = %v", err)
			c.Check(string(got) == `{"data":{"a":"after-ping"}}`, "next after ping = %s", got)
		})

		t.Run("DistinctIDsForIdenticalQueries", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)
			defer client.CloseSubscriptions()
			reqs := make([]*Request, 3)
			for i := range reqs {
				reqs[i] = NewRequest("subscription { same }")
			}
			for _, req := range reqs {
				if _, err := client.Subscribe(context.Background(), req, 1); err != nil {
					t.Fatalf("Subscribe: %v", err)
				}
			}
			s.connFor()
			if f := s.recv(); f.Type != "connection_init" {
				t.Fatalf("first frame type = %q, want connection_init", f.Type)
			}
			s.send("connection_ack", "", nil)
			seen := map[string]bool{}
			for i := 0; i < len(reqs); i++ {
				f := s.recv()
				checkFrame(c, f, "subscribe", "")
				c.Check(!seen[f.ID], "identical queries merged onto id %q", f.ID)
				seen[f.ID] = true
			}
		})

		t.Run("SlowConsumerIsolation", func(t *testing.T) {
			// Capacity 1 operation overflows; a capacity 4 operation on the
			// same connection keeps receiving.
			s := newSubServer(t)
			client := subscriptionClient(t, s)
			deferred := func() { client.CloseSubscriptions() }
			defer deferred()

			slow, err := client.Subscribe(context.Background(), NewRequest("subscription { slow }"), 1)
			c.Check(err == nil, "Subscribe slow: %v", err)
			full, err := client.Subscribe(context.Background(), NewRequest("subscription { full }"), 4)
			c.Check(err == nil, "Subscribe full: %v", err)
			s.connFor()
			if init := s.recv(); init.Type != "connection_init" {
				t.Fatalf("first frame type = %q, want connection_init", init.Type)
			}
			s.send("connection_ack", "", nil)
			slowFrame := s.recv()
			fullFrame := s.recv()

			// Slow result is accepted, then the queue overflows.
			s.sendNext(slowFrame.ID, `{"data":{"slow":1}}`)
			s.sendNext(slowFrame.ID, `{"data":{"slow":2}}`)

			// The slow operation must produce exactly one complete and it
			// must not stall the shared read loop.
			checkFrame(c, s.recv(), "complete", slowFrame.ID)
			for i := 0; i < 3; i++ {
				s.sendNext(fullFrame.ID, fmt.Sprintf(`{"data":{"full":%d}}`, i))
			}
			for i := 0; i < 3; i++ {
				got, err := nextWithin(t, full)
				c.Check(err == nil, "healthy subscription next %d error = %v", i, err)
				c.Check(string(got) == fmt.Sprintf(`{"data":{"full":%d}}`, i), "healthy payload %d = %s", i, got)
			}

			// Accepted result is still readable, then ErrSlowConsumer.
			got, err := nextWithin(t, slow)
			c.Check(err == nil, "accepted result error = %v", err)
			c.Check(string(got) == `{"data":{"slow":1}}`, "accepted result = %s", got)
			_, err = nextWithin(t, slow)
			c.Check(errors.Is(err, ErrSlowConsumer), "error after drain = %v, want ErrSlowConsumer", err)
			// Repeated termination is silent.
			slow.Cancel()
			extra := s.barrier()
			c.Check(len(extra) == 0, "Cancel after slow termination sent extra frames %+v", extra)
		})
	})
}

func TestSubscriptionTermination(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("CancelBeforeAck", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)
			defer client.CloseSubscriptions()

			ctx, cancel := context.WithCancel(context.Background())
			early, err := client.Subscribe(ctx, NewRequest("subscription { early }"), 2)
			c.Check(err == nil, "Subscribe early: %v", err)
			kept, err := client.Subscribe(context.Background(), NewRequest("subscription { kept }"), 2)
			c.Check(err == nil, "Subscribe kept: %v", err)

			// Cancel before the acknowledgement arrives.
			cancel()

			s.connFor()
			if f := s.recv(); f.Type != "connection_init" {
				t.Fatalf("first frame type = %q, want connection_init", f.Type)
			}
			s.send("connection_ack", "", nil)

			// Only the surviving operation is admitted.
			f := s.recv()
			checkFrame(c, f, "subscribe", kept.id)
			extra := s.barrier()
			c.Check(len(extra) == 0, "cancelled-before-ack operation sent frames %+v", extra)

			got, err := nextWithin(t, early)
			c.Check(errors.Is(err, context.Canceled), "cancelled operation next error = %v, want context.Canceled", err)
			c.Check(got == nil, "cancelled operation returned payload %s", got)

			// The context cancellation must not have closed the shared
			// connection: the surviving subscription still works.
			s.sendNext(kept.id, `{"data":{"kept":true}}`)
			got, err = nextWithin(t, kept)
			c.Check(err == nil, "surviving next error = %v", err)
			c.Check(string(got) == `{"data":{"kept":true}}`, "surviving payload = %s", got)
		})

		t.Run("CancelAfterAck", func(t *testing.T) {
			s, client, sub1, _, ids := ackedPair(t, 2)
			defer client.CloseSubscriptions()

			// The server processes frames in order, so the pong returned for a
			// ping sent after next proves the next payload was already
			// accepted by the read loop. No sleeps.
			s.sendNext(ids[0], `{"data":{"a":1}}`)
			s.writeRaw([]byte(`{"type":"ping","payload":{"barrier":true}}`))
			checkFrame(c, s.recv(), "pong", "")

			sub1.Cancel()
			checkFrame(c, s.recv(), "complete", ids[0])

			got, err := nextWithin(t, sub1)
			c.Check(err == nil, "accepted result before cancel error = %v", err)
			c.Check(string(got) == `{"data":{"a":1}}`, "accepted result = %s", got)
			_, err = nextWithin(t, sub1)
			c.Check(errors.Is(err, context.Canceled), "next after cancel = %v, want context.Canceled", err)

			sub1.Cancel()
			extra := s.barrier()
			c.Check(len(extra) == 0, "repeated Cancel sent extra frames %+v", extra)
		})

		t.Run("ServerErrorPayload", func(t *testing.T) {
			s, client, sub1, _, ids := ackedPair(t, 4)
			defer client.CloseSubscriptions()

			s.sendNext(ids[0], `{"data":{"partial":1}}`)
			s.send("error", ids[0], json.RawMessage(`[{"message":"boom"}]`))

			got, err := nextWithin(t, sub1)
			c.Check(err == nil, "payload before server error = %v", err)
			c.Check(string(got) == `{"data":{"partial":1}}`, "payload before server error = %s", got)

			_, err = nextWithin(t, sub1)
			var subErr *SubscriptionError
			c.Check(errors.As(err, &subErr), "error type = %T, want *SubscriptionError", err)
			if subErr != nil {
				c.Check(string(subErr.Payload) == `[{"message":"boom"}]`, "error payload = %s", subErr.Payload)
			}
			sub1.Cancel()
			extra := s.barrier()
			c.Check(len(extra) == 0, "Cancel after server error sent extra frames %+v", extra)
		})

		t.Run("ServerCompleteDrains", func(t *testing.T) {
			s, client, _, sub2, ids := ackedPair(t, 4)
			defer client.CloseSubscriptions()

			s.sendNext(ids[1], `{"data":{"b":1}}`)
			s.send("complete", ids[1], nil)

			got, err := nextWithin(t, sub2)
			c.Check(err == nil, "result before complete error = %v", err)
			c.Check(string(got) == `{"data":{"b":1}}`, "result before complete = %s", got)
			_, err = nextWithin(t, sub2)
			c.Check(errors.Is(err, io.EOF), "error after complete = %v, want io.EOF", err)
		})

		t.Run("NextContextOnlyInterruptsWait", func(t *testing.T) {
			s, client, sub1, _, ids := ackedPair(t, 2)
			defer client.CloseSubscriptions()

			waitCtx, cancelWait := context.WithTimeout(context.Background(), 100*time.Millisecond)
			_, err := sub1.Next(waitCtx)
			cancelWait()
			c.Check(errors.Is(err, context.DeadlineExceeded), "waiting Next error = %v, want DeadlineExceeded", err)

			// The subscription itself is still alive.
			s.sendNext(ids[0], `{"data":{"a":"later"}}`)
			got, err := nextWithin(t, sub1)
			c.Check(err == nil, "next after interrupted wait error = %v", err)
			c.Check(string(got) == `{"data":{"a":"later"}}`, "payload after interrupted wait = %s", got)
		})

		t.Run("MalformedAndUnknown", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)
			sub, err := client.Subscribe(context.Background(), NewRequest("subscription { n }"), 4)
			c.Check(err == nil, "Subscribe first: %v", err)
			other, err := client.Subscribe(context.Background(), NewRequest("subscription { m }"), 4)
			c.Check(err == nil, "Subscribe second: %v", err)
			s.connFor()
			s.recv() // connection_init
			s.send("connection_ack", "", nil)
			f := s.recv()
			otherFrame := s.recv()

			s.sendNext(f.ID, `{"data":{"n":1}}`)
			s.sendNext(otherFrame.ID, `{"data":{"m":1}}`)
			s.sendNext(f.ID, `{"data":{"n":2}}`)
			// A ping barrier guarantees all next messages were processed
			// before the malformed frame is written.
			s.writeRaw([]byte(`{"type":"ping","payload":{}}`))
			checkFrame(c, s.recv(), "pong", "")

			s.writeRaw([]byte(`not json`))
			for _, want := range []string{`{"data":{"n":1}}`, `{"data":{"n":2}}`} {
				got, err := nextWithin(t, sub)
				c.Check(err == nil, "accepted result before malformed frame error = %v", err)
				c.Check(string(got) == want, "accepted result = %s, want %s", got, want)
			}
			_, err = nextWithin(t, sub)
			c.Check(err != nil, "malformed frame did not terminate the subscription")
			c.Check(strings.Contains(err.Error(), "malformed"), "malformed frame reason = %v", err)
			// Every active operation terminates, keeping its accepted data.
			got, err := nextWithin(t, other)
			c.Check(err == nil, "other operation accepted result error = %v", err)
			c.Check(string(got) == `{"data":{"m":1}}`, "other operation accepted result = %s", got)
			_, err = nextWithin(t, other)
			c.Check(err != nil, "malformed frame did not terminate the other operation")

			// Connection is gone; no further operations are possible.
			_, err = client.Subscribe(context.Background(), NewRequest("subscription { n }"), 1)
			c.Check(err != nil, "Subscribe after broken connection error = %v", err)
			c.Check(client.CloseSubscriptions() == nil, "CloseSubscriptions after broken connection")
		})

		t.Run("UnknownMessageType", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)
			sub, err := client.Subscribe(context.Background(), NewRequest("subscription { n }"), 2)
			c.Check(err == nil, "Subscribe: %v", err)
			s.connFor()
			s.recv() // connection_init
			s.send("connection_ack", "", nil)
			s.recv() // subscribe

			s.writeRaw([]byte(`{"type":"bogus","id":"x"}`))
			_, err = nextWithin(t, sub)
			c.Check(err != nil, "unknown message type did not terminate the subscription")
			c.Check(strings.Contains(err.Error(), "unknown"), "unknown message reason = %v", err)
			c.Check(client.CloseSubscriptions() == nil, "CloseSubscriptions")
		})

		t.Run("ConnectionLoss", func(t *testing.T) {
			s := newSubServer(t)
			client := subscriptionClient(t, s)
			sub, err := client.Subscribe(context.Background(), NewRequest("subscription { n }"), 2)
			c.Check(err == nil, "Subscribe: %v", err)
			s.connFor()
			s.recv() // connection_init
			s.send("connection_ack", "", nil)
			s.recv() // subscribe

			s.sendNext(sub.id, `{"data":{"n":1}}`)
			s.Close()

			got, err := nextWithin(t, sub)
			c.Check(err == nil, "accepted result before connection loss = %v", err)
			c.Check(string(got) == `{"data":{"n":1}}`, "accepted result = %s", got)
			_, err = nextWithin(t, sub)
			c.Check(err != nil && !errors.Is(err, io.EOF), "connection loss error = %v", err)
			c.Check(client.CloseSubscriptions() == nil, "CloseSubscriptions after connection loss")
		})

		t.Run("LateMessagesIgnored", func(t *testing.T) {
			s, client, sub1, sub2, ids := ackedPair(t, 2)
			defer client.CloseSubscriptions()

			sub1.Cancel()
			checkFrame(c, s.recv(), "complete", ids[0])

			// Frames for the finished operation and an unknown id are ignored.
			s.sendNext(ids[0], `{"data":{"late":1}}`)
			s.sendNext("unknown-id", `{"data":{"nobody":1}}`)
			s.send("complete", ids[0], nil)

			_, err := nextWithin(t, sub1)
			c.Check(errors.Is(err, context.Canceled), "late next message reached a cancelled operation: %v", err)

			s.sendNext(ids[1], `{"data":{"still":true}}`)
			got, err := nextWithin(t, sub2)
			c.Check(err == nil, "other subscription after late messages error = %v", err)
			c.Check(string(got) == `{"data":{"still":true}}`, "other subscription payload = %s", got)
		})

		t.Run("CloseSubscriptionsSemantics", func(t *testing.T) {
			// The HTTP path remains usable alongside subscriptions.
			req := NewRequest("query { n }")
			c.Check(req.Query() == "query { n }", "ordinary request query changed")

			s := newSubServer(t)
			client := subscriptionClient(t, s)
			sub, err := client.Subscribe(context.Background(), NewRequest("subscription { n }"), 2)
			c.Check(err == nil, "Subscribe: %v", err)
			s.connFor()
			s.recv() // connection_init
			s.send("connection_ack", "", nil)
			s.recv() // subscribe
			s.sendNext(sub.id, `{"data":{"n":1}}`)
			// Barrier: the pong proves the accepted result reached the queue.
			s.writeRaw([]byte(`{"type":"ping","payload":{}}`))
			checkFrame(c, s.recv(), "pong", "")

			// CloseSubscriptions must not wait for the queue to be drained.
			c.Check(client.CloseSubscriptions() == nil, "CloseSubscriptions error")
			c.Check(client.CloseSubscriptions() == nil, "second CloseSubscriptions error")
			_, err = client.Subscribe(context.Background(), NewRequest("subscription { n }"), 1)
			c.Check(errors.Is(err, ErrSubscriptionsClosed), "Subscribe after close = %v, want ErrSubscriptionsClosed", err)

			// Accepted data is preserved and readable after close.
			got, err := nextWithin(t, sub)
			c.Check(err == nil, "accepted result after close error = %v", err)
			c.Check(string(got) == `{"data":{"n":1}}`, "accepted result after close = %s", got)
			_, err = nextWithin(t, sub)
			c.Check(errors.Is(err, ErrSubscriptionsClosed), "next after close = %v, want ErrSubscriptionsClosed", err)
		})
	})
}
