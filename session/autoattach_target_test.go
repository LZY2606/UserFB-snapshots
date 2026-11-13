package session_test

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/protocol/runtime"
	"github.com/mafredri/cdp/protocol/target"
	"github.com/mafredri/cdp/rpcc"
	"github.com/mafredri/cdp/session"
)

// rpcRequest is a request decoded from the (parent or session) wire.
type rpcRequest struct {
	ID     uint64          `json:"id"`
	Method string          `json:"method"`
	Params json.RawMessage `json:"params"`
}

// call records one command seen by the fake CDP peer.
type call struct {
	method    string
	sessionID target.SessionID // Empty for parent-level commands.
	inner     string           // Method of the nested session message, if any.
	id        uint64           // Request ID (inner ID for session messages).
	params    json.RawMessage  // Parent-level command params.
}

func (c call) String() string {
	if c.sessionID != "" {
		return c.method + "[" + string(c.sessionID) + "]/" + c.inner
	}
	return c.method
}

// behavior tells the fake peer how to answer one command.
type behavior struct {
	// reply is the result payload (nil means an empty object result).
	reply interface{}
	// responseError, when non-nil, is returned as an RPC error instead
	// of a result.
	responseError *rpcc.ResponseError
	// hold keeps the reply until the channel is closed, giving the
	// test explicit control over when the command completes.
	hold <-chan struct{}
	// arrived runs on the server goroutine when the request arrives,
	// before waiting on hold. It must only be attached to behaviors
	// used once.
	arrived func()
}

// fakeCDP is a minimal CDP peer speaking JSON-RPC over net.Pipe. It
// independently controls the setAutoAttach confirmation, per-target
// Runtime.evaluate replies, the resume confirmation and detach events.
type fakeCDP struct {
	t       *testing.T
	server  net.Conn
	dec     *json.Decoder
	encMu   sync.Mutex
	enc     *json.Encoder
	callsMu sync.Mutex
	calls   []call

	mu        sync.Mutex
	behaviors map[string]*behavior // Keyed by method, or "method/sessionID/inner".
}

func newFakeCDP(t *testing.T, server net.Conn) *fakeCDP {
	t.Helper()
	f := &fakeCDP{
		t:         t,
		server:    server,
		dec:       json.NewDecoder(server),
		enc:       json.NewEncoder(server),
		behaviors: make(map[string]*behavior),
	}
	t.Cleanup(func() { _ = server.Close() })
	return f
}

// serve decodes requests until the connection closes.
func (f *fakeCDP) serve() {
	for {
		var req rpcRequest
		if err := f.dec.Decode(&req); err != nil {
			return
		}
		f.handle(req)
	}
}

func (f *fakeCDP) handle(req rpcRequest) {
	switch req.Method {
	case "Target.setAutoAttach", "Target.attachToTarget", "Target.detachFromTarget":
		f.record(call{method: req.Method, id: req.ID, params: req.Params})
		f.answer(req.ID, req.Method, nil)
	case "Target.sendMessageToTarget":
		var args struct {
			SessionID string `json:"sessionId"`
			Message   string `json:"message"`
		}
		if err := json.Unmarshal(req.Params, &args); err != nil {
			f.t.Errorf("fakeCDP: bad sendMessageToTarget params: %v", err)
			return
		}
		var inner rpcRequest
		if err := json.Unmarshal([]byte(args.Message), &inner); err != nil {
			f.t.Errorf("fakeCDP: bad nested message: %v", err)
			return
		}
		sid := target.SessionID(args.SessionID)
		f.record(call{
			method:    req.Method,
			sessionID: sid,
			inner:     inner.Method,
			id:        inner.ID,
		})
		f.answer(req.ID, req.Method, nil) // Parent-level send ack.

		// Deliver the nested reply through the session event channel;
		// the Manager tunnels it back to the child rpcc.Conn.
		key := req.Method + "/" + args.SessionID + "/" + inner.Method
		go func() {
			if err := f.answerSession(sid, inner, key); err != nil && !isClosedErr(err) {
				f.t.Errorf("fakeCDP: session reply %s failed: %v", key, err)
			}
		}()
	default:
		f.record(call{method: req.Method, id: req.ID, params: req.Params})
		f.answer(req.ID, req.Method, nil)
	}
}

// answer responds to a parent-level request.
func (f *fakeCDP) answer(id uint64, method string, defaultReply interface{}) {
	b := f.behavior(method)
	if b != nil && b.arrived != nil {
		b.arrived()
	}
	if b != nil && b.hold != nil {
		<-b.hold
	}
	f.encMu.Lock()
	defer f.encMu.Unlock()
	if b != nil && b.responseError != nil {
		_ = f.enc.Encode(map[string]interface{}{
			"id":    id,
			"error": b.responseError,
		})
		return
	}
	reply := defaultReply
	if b != nil && b.reply != nil {
		reply = b.reply
	}
	if reply == nil {
		reply = map[string]interface{}{}
	}
	_ = f.enc.Encode(map[string]interface{}{
		"id":     id,
		"result": reply,
	})
}

// answerSession sends the nested response wrapped in a
// receivedMessageFromTarget event.
func (f *fakeCDP) answerSession(sid target.SessionID, inner rpcRequest, key string) error {
	b := f.behavior(key)
	if b != nil && b.arrived != nil {
		b.arrived()
	}
	if b != nil && b.hold != nil {
		<-b.hold
	}

	var nested json.RawMessage
	if b != nil && b.responseError != nil {
		nested, _ = json.Marshal(map[string]interface{}{
			"id":    inner.ID,
			"error": b.responseError,
		})
	} else {
		reply := interface{}(map[string]interface{}{})
		if b != nil && b.reply != nil {
			reply = b.reply
		}
		nested, _ = json.Marshal(map[string]interface{}{
			"id":     inner.ID,
			"result": reply,
		})
	}

	event := map[string]interface{}{
		"method": "Target.receivedMessageFromTarget",
		"params": map[string]interface{}{
			"sessionId": string(sid),
			"message":   string(nested),
		},
	}
	f.encMu.Lock()
	defer f.encMu.Unlock()
	return f.enc.Encode(event)
}

func (f *fakeCDP) behavior(key string) *behavior {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.behaviors[key]
}

// setBehavior installs a reply rule. Key is a plain method name for
// parent commands or "sendMessageToTarget/<session>/<inner>" for
// session commands.
func (f *fakeCDP) setBehavior(key string, b *behavior) {
	f.mu.Lock()
	f.behaviors[key] = b
	f.mu.Unlock()
}

func (f *fakeCDP) record(c call) {
	f.callsMu.Lock()
	f.calls = append(f.calls, c)
	f.callsMu.Unlock()
}

// recorded returns a copy of all commands seen so far, in order.
func (f *fakeCDP) recorded() []call {
	f.callsMu.Lock()
	defer f.callsMu.Unlock()
	out := make([]call, len(f.calls))
	copy(out, f.calls)
	return out
}

// waitFor blocks until a call matching pred has been recorded.
func (f *fakeCDP) waitFor(t *testing.T, what string, pred func(call) bool) call {
	t.Helper()
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		for _, c := range f.recorded() {
			if pred(c) {
				return c
			}
		}
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("timed out waiting for %s; recorded: %v", what, f.recorded())
	return call{}
}

// sendAttached emits an attachedToTarget event.
func (f *fakeCDP) sendAttached(sessionID target.SessionID, targetID target.ID, waiting bool) error {
	event := map[string]interface{}{
		"method": "Target.attachedToTarget",
		"params": map[string]interface{}{
			"sessionId": string(sessionID),
			"targetInfo": map[string]interface{}{
				"targetId":        string(targetID),
				"type":            "worker",
				"title":           "",
				"url":             "https://example.test/worker.js",
				"attached":        true,
				"canAccessOpener": false,
			},
			"waitingForDebugger": waiting,
		},
	}
	f.encMu.Lock()
	defer f.encMu.Unlock()
	return f.enc.Encode(event)
}

// sendDetached emits a detachedFromTarget event.
func (f *fakeCDP) sendDetached(sessionID target.SessionID) error {
	event := map[string]interface{}{
		"method": "Target.detachedFromTarget",
		"params": map[string]interface{}{
			"sessionId": string(sessionID),
		},
	}
	f.encMu.Lock()
	defer f.encMu.Unlock()
	return f.enc.Encode(event)
}

func isClosedErr(err error) bool {
	return errors.Is(err, net.ErrClosed) || errors.Is(err, io.ErrClosedPipe)
}

// testHarness bundles a fake peer, parent rpcc.Conn and Manager.
type testHarness struct {
	t       *testing.T
	fake    *fakeCDP
	conn    *rpcc.Conn
	manager *session.Manager
}

func newHarness(t *testing.T) *testHarness {
	t.Helper()
	client, server := net.Pipe()
	fake := newFakeCDP(t, server)
	go fake.serve()

	conn, err := rpcc.DialContext(context.Background(), "",
		rpcc.WithDialer(func(context.Context, string) (io.ReadWriteCloser, error) {
			return client, nil
		}))
	if err != nil {
		t.Fatalf("rpcc.DialContext: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })

	m, err := session.NewManager(cdp.NewClient(conn))
	if err != nil {
		t.Fatalf("session.NewManager: %v", err)
	}
	t.Cleanup(func() { _ = m.Close() })

	return &testHarness{t: t, fake: fake, conn: conn, manager: m}
}

// evalResult is the Runtime.evaluate result used by default session
// replies.
func evalResult(value int) interface{} {
	return map[string]interface{}{
		"result": map[string]interface{}{
			"type":  "number",
			"value": value,
		},
	}
}

func sessionKey(sid target.SessionID, inner string) string {
	return "Target.sendMessageToTarget/" + string(sid) + "/" + inner
}

// TestAutoAttachEnable covers the enabling handshake itself: command
// parameters, at-most-once semantics, error propagation and the
// event-before-confirmation ordering.
func TestAutoAttachEnable(t *testing.T) {
	t.Run("command parameters and confirmation", func(t *testing.T) {
		h := newHarness(t)

		confirm := make(chan struct{})
		h.fake.setBehavior("Target.setAutoAttach", &behavior{hold: confirm})
		ready := make(chan target.Info, 1)

		errC := make(chan error, 1)
		go func() {
			errC <- h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
				Ready: func(info target.Info, _ *rpcc.Conn) { ready <- info },
			})
		}()

		// The enable call must wait for the command result.
		select {
		case err := <-errC:
			t.Fatalf("AutoAttach returned before confirmation: %v", err)
		case <-time.After(50 * time.Millisecond):
		}

		setCall := h.fake.waitFor(t, "setAutoAttach", func(c call) bool {
			return c.method == "Target.setAutoAttach"
		})
		var params struct {
			AutoAttach             bool  `json:"autoAttach"`
			WaitForDebuggerOnStart bool  `json:"waitForDebuggerOnStart"`
			Flatten                *bool `json:"flatten"`
		}
		if err := json.Unmarshal(setCall.params, &params); err != nil {
			t.Fatalf("bad setAutoAttach params: %v", err)
		}
		if !params.AutoAttach || !params.WaitForDebuggerOnStart ||
			params.Flatten == nil || *params.Flatten {
			t.Fatalf("unexpected setAutoAttach params: %+v", params)
		}

		// An attach event arriving before the reply must not trigger
		// initialization early.
		if err := h.fake.sendAttached("s-early", "t-early", true); err != nil {
			t.Fatal(err)
		}
		select {
		case info := <-ready:
			t.Fatalf("target initialized before confirmation: %+v", info)
		case <-time.After(50 * time.Millisecond):
		}

		close(confirm)
		select {
		case err := <-errC:
			if err != nil {
				t.Fatalf("AutoAttach: %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("AutoAttach did not return after confirmation")
		}

		// The early target initializes only after confirmation.
		select {
		case info := <-ready:
			if info.TargetID != "t-early" {
				t.Fatalf("got target %q, want t-early", info.TargetID)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("early target was not taken over after confirmation")
		}

		// Resume must have been sent because waitingForDebugger=true.
		h.fake.waitFor(t, "runIfWaitingForDebugger", func(c call) bool {
			return c.sessionID == "s-early" && c.inner == "Runtime.runIfWaitingForDebugger"
		})
	})

	t.Run("repeated calls return ErrAutoAttachActive", func(t *testing.T) {
		h := newHarness(t)

		confirm := make(chan struct{})
		h.fake.setBehavior("Target.setAutoAttach", &behavior{hold: confirm})

		errs := make(chan error, 1)
		go func() {
			errs <- h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{})
		}()
		h.fake.waitFor(t, "setAutoAttach", func(c call) bool {
			return c.method == "Target.setAutoAttach"
		})

		// A second call while enabling is in progress.
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{}); !errors.Is(err, session.ErrAutoAttachActive) {
			t.Fatalf("concurrent AutoAttach error = %v, want ErrAutoAttachActive", err)
		}

		close(confirm)
		if err := <-errs; err != nil {
			t.Fatalf("first AutoAttach: %v", err)
		}

		// And again after the first call succeeded.
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{}); !errors.Is(err, session.ErrAutoAttachActive) {
			t.Fatalf("repeated AutoAttach error = %v, want ErrAutoAttachActive", err)
		}
	})

	t.Run("rejected command returns response error and detaches targets", func(t *testing.T) {
		h := newHarness(t)

		rpcErr := &rpcc.ResponseError{Code: -32601, Message: "not supported"}
		confirm := make(chan struct{})
		h.fake.setBehavior("Target.setAutoAttach", &behavior{
			hold:          confirm,
			responseError: rpcErr,
		})

		started := make(chan error, 1)
		go func() {
			started <- h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
				Init: func(context.Context, target.Info, *cdp.Client) error {
					t.Error("Init must not run when enabling fails")
					return nil
				},
				Ready: func(target.Info, *rpcc.Conn) {
					t.Error("Ready must not run when enabling fails")
				},
			})
		}()
		h.fake.waitFor(t, "setAutoAttach", func(c call) bool {
			return c.method == "Target.setAutoAttach"
		})

		// Two targets attach before the rejection is delivered.
		if err := h.fake.sendAttached("s-a", "t-a", true); err != nil {
			t.Fatal(err)
		}
		if err := h.fake.sendAttached("s-b", "t-b", false); err != nil {
			t.Fatal(err)
		}

		close(confirm)
		select {
		case err := <-started:
			var got *rpcc.ResponseError
			if !errors.As(err, &got) || got.Code != rpcErr.Code {
				t.Fatalf("AutoAttach error = %v, want ResponseError %v", err, rpcErr)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("AutoAttach did not return after rejection")
		}

		// Both pre-confirmation sessions must be detached, and no
		// AttachToTarget may have been sent for them.
		deadline := time.Now().Add(2 * time.Second)
		var detaches []call
		for time.Now().Before(deadline) {
			detaches = nil
			for _, c := range h.fake.recorded() {
				if c.method == "Target.detachFromTarget" {
					detaches = append(detaches, c)
				}
			}
			if len(detaches) >= 2 {
				break
			}
			time.Sleep(time.Millisecond)
		}
		if len(detaches) != 2 {
			t.Fatalf("got %d detaches, want 2: %v", len(detaches), h.fake.recorded())
		}
		detached := map[target.SessionID]bool{}
		for _, c := range detaches {
			var args struct {
				SessionID string `json:"sessionId"`
			}
			if err := json.Unmarshal(c.params, &args); err != nil {
				t.Fatalf("bad detach params: %v", err)
			}
			detached[target.SessionID(args.SessionID)] = true
		}
		if !detached["s-a"] || !detached["s-b"] {
			t.Fatalf("detached sessions = %v, want s-a and s-b", detached)
		}
		for _, c := range h.fake.recorded() {
			if c.method == "Target.attachToTarget" {
				t.Fatalf("auto-attached session must not be attached again: %v", c)
			}
		}
	})

	t.Run("context cancellation before confirmation detaches targets", func(t *testing.T) {
		h := newHarness(t)

		confirm := make(chan struct{})
		h.fake.setBehavior("Target.setAutoAttach", &behavior{hold: confirm})
		ctx, cancel := context.WithCancel(context.Background())

		errC := make(chan error, 1)
		go func() { errC <- h.manager.AutoAttach(ctx, session.AutoAttachOptions{}) }()
		h.fake.waitFor(t, "setAutoAttach", func(c call) bool {
			return c.method == "Target.setAutoAttach"
		})
		if err := h.fake.sendAttached("s-c", "t-c", true); err != nil {
			t.Fatal(err)
		}

		cancel()
		select {
		case err := <-errC:
			if !errors.Is(err, context.Canceled) {
				t.Fatalf("AutoAttach error = %v, want context.Canceled", err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("AutoAttach did not return after context cancellation")
		}
		close(confirm) // Unblock the fake peer so it can read the detach.

		h.fake.waitFor(t, "detach after cancel", func(c call) bool {
			return c.method == "Target.detachFromTarget"
		})
	})
}

// TestAutoAttachStartup covers the takeover sequence inside targets.
func TestAutoAttachStartup(t *testing.T) {
	t.Run("Init evaluate is served while dispatch continues", func(t *testing.T) {
		h := newHarness(t)

		evalStarted := make(chan struct{})
		evalRelease := make(chan struct{})
		h.fake.setBehavior(sessionKey("s1", "Runtime.evaluate"), &behavior{
			reply:   evalResult(11),
			hold:    evalRelease,
			arrived: func() { close(evalStarted) },
		})

		ready := make(chan target.ID, 2)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(ctx context.Context, _ target.Info, c *cdp.Client) error {
				reply, err := c.Runtime.Evaluate(ctx,
					runtime.NewEvaluateArgs(`1+10`).SetReturnByValue(true))
				if err != nil {
					return err
				}
				if string(reply.Result.Value) != "11" {
					return fmt.Errorf("unexpected value %s", reply.Result.Value)
				}
				return nil
			},
			Ready: func(info target.Info, _ *rpcc.Conn) { ready <- info.TargetID },
		}); err != nil {
			t.Fatal(err)
		}

		if err := h.fake.sendAttached("s1", "t1", true); err != nil {
			t.Fatal(err)
		}
		<-evalStarted // The Init evaluate reached the peer.

		// s1's evaluate reply is held. A second target still runs the
		// full attach -> Init -> resume -> Ready path, proving the
		// Manager dispatch loop is not occupied by s1's Init.
		h.fake.setBehavior(sessionKey("s2", "Runtime.evaluate"), &behavior{reply: evalResult(11)})
		if err := h.fake.sendAttached("s2", "t2", true); err != nil {
			t.Fatal(err)
		}
		select {
		case id := <-ready:
			if id != "t2" {
				t.Fatalf("first Ready target = %q, want t2", id)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("s2 did not become Ready while s1's Init was blocked")
		}

		close(evalRelease)
		select {
		case id := <-ready:
			if id != "t1" {
				t.Fatalf("second Ready target = %q, want t1", id)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("target s1 never became Ready")
		}

		// Per-session command order: Evaluate precedes the resume.
		var s1Order []string
		for _, c := range h.fake.recorded() {
			if c.sessionID == "s1" {
				s1Order = append(s1Order, c.inner)
			}
		}
		want := []string{"Runtime.evaluate", "Runtime.runIfWaitingForDebugger"}
		if fmt.Sprint(s1Order) != fmt.Sprint(want) {
			t.Fatalf("s1 command order = %v, want %v", s1Order, want)
		}
	})

	t.Run("two targets initialize concurrently and interleave", func(t *testing.T) {
		h := newHarness(t)

		s1Eval := make(chan struct{})
		s2Resume := make(chan struct{})
		h.fake.setBehavior(sessionKey("s1", "Runtime.evaluate"), &behavior{
			reply: evalResult(1), hold: s1Eval,
		})
		h.fake.setBehavior(sessionKey("s2", "Runtime.evaluate"), &behavior{
			reply: evalResult(2),
		})
		h.fake.setBehavior(sessionKey("s2", "Runtime.runIfWaitingForDebugger"), &behavior{
			hold: s2Resume,
		})

		ready := make(chan target.ID, 2)
		opts := session.AutoAttachOptions{
			Init: func(ctx context.Context, _ target.Info, c *cdp.Client) error {
				_, err := c.Runtime.Evaluate(ctx,
					runtime.NewEvaluateArgs(`1`).SetReturnByValue(true))
				return err
			},
			Ready: func(info target.Info, _ *rpcc.Conn) {
				ready <- info.TargetID
			},
		}
		if err := h.manager.AutoAttach(context.Background(), opts); err != nil {
			t.Fatal(err)
		}

		if err := h.fake.sendAttached("s1", "tid1", true); err != nil {
			t.Fatal(err)
		}
		if err := h.fake.sendAttached("s2", "tid2", true); err != nil {
			t.Fatal(err)
		}

		// s1's evaluate is held, yet s2 runs all the way to its held
		// resume: initialization of different targets is concurrent.
		h.fake.waitFor(t, "s2 evaluate", func(c call) bool {
			return c.sessionID == "s2" && c.inner == "Runtime.evaluate"
		})
		close(s2Resume)
		select {
		case id := <-ready:
			if id != "tid2" {
				t.Fatalf("first Ready target = %q, want tid2", id)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("s2 did not become Ready while s1 was held")
		}

		close(s1Eval)
		select {
		case id := <-ready:
			if id != "tid1" {
				t.Fatalf("second Ready target = %q, want tid1", id)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("s1 did not become Ready after release")
		}
	})

	t.Run("duplicate attach event initializes once", func(t *testing.T) {
		h := newHarness(t)
		h.fake.setBehavior(sessionKey("sd", "Runtime.evaluate"), &behavior{reply: evalResult(1)})

		ready := make(chan struct{}, 4)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(ctx context.Context, _ target.Info, c *cdp.Client) error {
				_, err := c.Runtime.Evaluate(ctx,
					runtime.NewEvaluateArgs(`1`).SetReturnByValue(true))
				return err
			},
			Ready: func(target.Info, *rpcc.Conn) { ready <- struct{}{} },
		}); err != nil {
			t.Fatal(err)
		}

		for i := 0; i < 3; i++ {
			if err := h.fake.sendAttached("sd", "td", true); err != nil {
				t.Fatal(err)
			}
		}
		select {
		case <-ready:
		case <-time.After(5 * time.Second):
			t.Fatal("target never became Ready")
		}
		// No second Ready is allowed.
		select {
		case <-ready:
			t.Fatal("duplicate attach produced a second Ready")
		case <-time.After(100 * time.Millisecond):
		}
		var evaluates int
		for _, c := range h.fake.recorded() {
			if c.sessionID == "sd" && c.inner == "Runtime.evaluate" {
				evaluates++
			}
		}
		if evaluates != 1 {
			t.Fatalf("got %d evaluates for duplicate attach, want 1", evaluates)
		}
	})

	t.Run("waitingForDebugger false skips resume", func(t *testing.T) {
		h := newHarness(t)
		h.fake.setBehavior(sessionKey("sn", "Runtime.evaluate"), &behavior{reply: evalResult(3)})

		ready := make(chan struct{}, 1)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(ctx context.Context, _ target.Info, c *cdp.Client) error {
				_, err := c.Runtime.Evaluate(ctx,
					runtime.NewEvaluateArgs(`1+2`).SetReturnByValue(true))
				return err
			},
			Ready: func(target.Info, *rpcc.Conn) { ready <- struct{}{} },
		}); err != nil {
			t.Fatal(err)
		}

		if err := h.fake.sendAttached("sn", "tn", false); err != nil {
			t.Fatal(err)
		}
		select {
		case <-ready:
		case <-time.After(5 * time.Second):
			t.Fatal("target never became Ready")
		}
		for _, c := range h.fake.recorded() {
			if c.inner == "Runtime.runIfWaitingForDebugger" {
				t.Fatalf("resume must not be sent when waitingForDebugger=false: %v", c)
			}
		}
	})
}

// TestAutoAttachIsolation covers failures, detach races and cleanup.
func TestAutoAttachIsolation(t *testing.T) {
	t.Run("Init failure publishes AutoAttachError and detaches", func(t *testing.T) {
		h := newHarness(t)
		initErr := errors.New("boom: init failed")

		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(context.Context, target.Info, *cdp.Client) error { return initErr },
		}); err != nil {
			t.Fatal(err)
		}
		if err := h.fake.sendAttached("sf", "tf", true); err != nil {
			t.Fatal(err)
		}

		select {
		case err := <-h.manager.Err():
			var ae *session.AutoAttachError
			if !errors.As(err, &ae) {
				t.Fatalf("Manager.Err = %T, want *AutoAttachError", err)
			}
			if ae.SessionID != "sf" || ae.TargetID != "tf" || ae.Phase != "init" {
				t.Fatalf("AutoAttachError = %+v", ae)
			}
			if !errors.Is(err, initErr) {
				t.Fatalf("Init error not recoverable via errors.Is: %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("no AutoAttachError published")
		}
		h.fake.waitFor(t, "detach after init failure", func(c call) bool {
			return c.method == "Target.detachFromTarget"
		})
		for _, c := range h.fake.recorded() {
			if c.inner == "Runtime.runIfWaitingForDebugger" {
				t.Fatal("resume must not run after Init failure")
			}
		}
	})

	t.Run("resume failure reports ResponseError and phase resume", func(t *testing.T) {
		h := newHarness(t)
		rpcErr := &rpcc.ResponseError{Code: -32000, Message: "resume rejected"}
		h.fake.setBehavior(sessionKey("sr", "Runtime.runIfWaitingForDebugger"), &behavior{
			responseError: rpcErr,
		})

		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(context.Context, target.Info, *cdp.Client) error { return nil },
		}); err != nil {
			t.Fatal(err)
		}
		if err := h.fake.sendAttached("sr", "tr", true); err != nil {
			t.Fatal(err)
		}

		select {
		case err := <-h.manager.Err():
			var ae *session.AutoAttachError
			if !errors.As(err, &ae) || ae.Phase != "resume" {
				t.Fatalf("Manager.Err = %v, want resume-phase AutoAttachError", err)
			}
			var got *rpcc.ResponseError
			if !errors.As(err, &got) || got.Code != rpcErr.Code {
				t.Fatalf("ResponseError not recoverable via errors.As: %v", err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("no resume error published")
		}
		h.fake.waitFor(t, "detach after resume failure", func(c call) bool {
			return c.method == "Target.detachFromTarget"
		})
	})

	t.Run("detach during Init cancels context and skips Ready", func(t *testing.T) {
		h := newHarness(t)

		evalArrived := make(chan struct{})
		releaseEval := make(chan struct{})
		h.fake.setBehavior(sessionKey("sd1", "Runtime.evaluate"), &behavior{
			reply:   evalResult(1),
			hold:    releaseEval,
			arrived: func() { close(evalArrived) },
		})

		ctxDone := make(chan struct{}, 1)
		ready := make(chan struct{}, 1)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(ctx context.Context, _ target.Info, c *cdp.Client) error {
				go func() {
					<-ctx.Done()
					ctxDone <- struct{}{}
				}()
				_, err := c.Runtime.Evaluate(ctx,
					runtime.NewEvaluateArgs(`1`).SetReturnByValue(true))
				return err
			},
			Ready: func(target.Info, *rpcc.Conn) { ready <- struct{}{} },
		}); err != nil {
			t.Fatal(err)
		}

		if err := h.fake.sendAttached("sd1", "td1", true); err != nil {
			t.Fatal(err)
		}
		<-evalArrived

		if err := h.fake.sendDetached("sd1"); err != nil {
			t.Fatal(err)
		}
		select {
		case <-ctxDone:
		case <-time.After(5 * time.Second):
			t.Fatal("Init context was not canceled on detach")
		}

		close(releaseEval) // Late evaluate reply must not revive the target.
		select {
		case <-ready:
			t.Fatal("Ready fired after detach during Init")
		case <-time.After(100 * time.Millisecond):
		}
	})

	t.Run("detach while resume is in flight suppresses late Ready", func(t *testing.T) {
		h := newHarness(t)

		resumeArrived := make(chan struct{})
		releaseResume := make(chan struct{})
		h.fake.setBehavior(sessionKey("sd2", "Runtime.runIfWaitingForDebugger"), &behavior{
			hold:    releaseResume,
			arrived: func() { close(resumeArrived) },
		})

		ready := make(chan struct{}, 1)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init:  func(context.Context, target.Info, *cdp.Client) error { return nil },
			Ready: func(target.Info, *rpcc.Conn) { ready <- struct{}{} },
		}); err != nil {
			t.Fatal(err)
		}

		if err := h.fake.sendAttached("sd2", "td2", true); err != nil {
			t.Fatal(err)
		}
		<-resumeArrived
		if err := h.fake.sendDetached("sd2"); err != nil {
			t.Fatal(err)
		}
		// Wait until the Manager has processed the detach and sent the
		// detach command, then deliver the held resume reply.
		h.fake.waitFor(t, "detachFromTarget command", func(c call) bool {
			return c.method == "Target.detachFromTarget"
		})
		close(releaseResume)

		select {
		case <-ready:
			t.Fatal("Ready fired from a late resume reply after detach")
		case <-time.After(100 * time.Millisecond):
		}
	})

	t.Run("one target failing does not break a sibling", func(t *testing.T) {
		h := newHarness(t)
		h.fake.setBehavior(sessionKey("sok", "Runtime.evaluate"), &behavior{reply: evalResult(9)})

		ready := make(chan target.ID, 1)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(ctx context.Context, info target.Info, c *cdp.Client) error {
				if info.TargetID == "tbad" {
					return errors.New("sibling failure")
				}
				_, err := c.Runtime.Evaluate(ctx,
					runtime.NewEvaluateArgs(`9`).SetReturnByValue(true))
				return err
			},
			Ready: func(info target.Info, _ *rpcc.Conn) { ready <- info.TargetID },
		}); err != nil {
			t.Fatal(err)
		}

		if err := h.fake.sendAttached("sbad", "tbad", true); err != nil {
			t.Fatal(err)
		}
		if err := h.fake.sendAttached("sok", "tok", true); err != nil {
			t.Fatal(err)
		}

		// Drain the failure.
		select {
		case <-h.manager.Err():
		case <-time.After(5 * time.Second):
			t.Fatal("expected AutoAttachError for failing sibling")
		}
		// The healthy target still reaches Ready.
		select {
		case id := <-ready:
			if id != "tok" {
				t.Fatalf("sibling Ready = %q, want tok", id)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("healthy sibling was not taken over")
		}
	})

	t.Run("attach then detach before confirmation is never initialized", func(t *testing.T) {
		h := newHarness(t)

		confirm := make(chan struct{})
		h.fake.setBehavior("Target.setAutoAttach", &behavior{hold: confirm})

		initRan := make(chan struct{}, 1)
		ready := make(chan struct{}, 1)
		errC := make(chan error, 1)
		go func() {
			errC <- h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
				Init: func(context.Context, target.Info, *cdp.Client) error {
					initRan <- struct{}{}
					return nil
				},
				Ready: func(target.Info, *rpcc.Conn) { ready <- struct{}{} },
			})
		}()
		h.fake.waitFor(t, "setAutoAttach", func(c call) bool {
			return c.method == "Target.setAutoAttach"
		})

		if err := h.fake.sendAttached("sp", "tp", true); err != nil {
			t.Fatal(err)
		}
		// The browser detaches again before the enable command is
		// confirmed.
		if err := h.fake.sendDetached("sp"); err != nil {
			t.Fatal(err)
		}
		close(confirm)
		if err := <-errC; err != nil {
			t.Fatalf("AutoAttach: %v", err)
		}

		select {
		case <-initRan:
			t.Fatal("Init ran for a target detached before confirmation")
		case <-time.After(100 * time.Millisecond):
		}
		select {
		case <-ready:
			t.Fatal("Ready ran for a target detached before confirmation")
		case <-time.After(100 * time.Millisecond):
		}
		for _, c := range h.fake.recorded() {
			if c.sessionID == "sp" {
				t.Fatalf("no session command expected for pre-confirmation detach, got %v", c)
			}
		}
	})

	t.Run("cleanup failure is preserved alongside the takeover error", func(t *testing.T) {
		h := newHarness(t)
		initErr := errors.New("init failed again")
		detachErr := &rpcc.ResponseError{Code: -32001, Message: "detach failed"}
		h.fake.setBehavior("Target.detachFromTarget", &behavior{responseError: detachErr})

		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init: func(context.Context, target.Info, *cdp.Client) error { return initErr },
		}); err != nil {
			t.Fatal(err)
		}
		if err := h.fake.sendAttached("sc", "tc", true); err != nil {
			t.Fatal(err)
		}

		// First error: the takeover failure.
		select {
		case err := <-h.manager.Err():
			var ae *session.AutoAttachError
			if !errors.As(err, &ae) || ae.Phase != "init" || !errors.Is(err, initErr) {
				t.Fatalf("first error = %v, want init-phase AutoAttachError wrapping initErr", err)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("missing AutoAttachError")
		}

		// Second error: the failed cleanup, still carrying the
		// protocol ResponseError.
		select {
		case err := <-h.manager.Err():
			var got *rpcc.ResponseError
			if !errors.As(err, &got) || got.Code != detachErr.Code {
				t.Fatalf("cleanup error = %v, want ResponseError %v", err, detachErr)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("missing cleanup error")
		}
	})

	t.Run("manual Dial is not taken over by auto-attach", func(t *testing.T) {
		h := newHarness(t)

		// The browser assigns "s-manual" for the manual attach and
		// emits an attachedToTarget event of its own.
		h.fake.setBehavior("Target.attachToTarget", &behavior{
			reply: map[string]interface{}{"sessionId": "s-manual"},
		})

		ready := make(chan target.ID, 1)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init:  func(context.Context, target.Info, *cdp.Client) error { return nil },
			Ready: func(info target.Info, _ *rpcc.Conn) { ready <- info.TargetID },
		}); err != nil {
			t.Fatal(err)
		}

		dialConn, err := h.manager.Dial(context.Background(), target.ID("t-manual"))
		if err != nil {
			t.Fatalf("Dial: %v", err)
		}
		defer dialConn.Close()

		// Simulate the browser's own attachedToTarget for the manual
		// session; auto-attach must ignore it.
		if err := h.fake.sendAttached("s-manual", "t-manual", false); err != nil {
			t.Fatal(err)
		}

		// A genuinely auto-attached target still works.
		if err := h.fake.sendAttached("s-auto", "t-auto", false); err != nil {
			t.Fatal(err)
		}
		select {
		case id := <-ready:
			if id != "t-auto" {
				t.Fatalf("Ready target = %q, want only t-auto (manual Dial ignored)", id)
			}
		case <-time.After(5 * time.Second):
			t.Fatal("auto-attached target never became Ready")
		}
		select {
		case id := <-ready:
			t.Fatalf("manual Dial target was taken over by auto-attach: %q", id)
		case <-time.After(100 * time.Millisecond):
		}

		// The manual session stays connected and usable.
		select {
		case <-dialConn.Context().Done():
			t.Fatal("manual Dial session closed unexpectedly")
		default:
		}
	})

	t.Run("Manager close closes auto sessions and ends Err channel", func(t *testing.T) {
		h := newHarness(t)

		readyConn := make(chan *rpcc.Conn, 1)
		if err := h.manager.AutoAttach(context.Background(), session.AutoAttachOptions{
			Init:  func(context.Context, target.Info, *cdp.Client) error { return nil },
			Ready: func(_ target.Info, conn *rpcc.Conn) { readyConn <- conn },
		}); err != nil {
			t.Fatal(err)
		}
		if err := h.fake.sendAttached("sx", "tx", false); err != nil {
			t.Fatal(err)
		}
		var child *rpcc.Conn
		select {
		case child = <-readyConn:
		case <-time.After(5 * time.Second):
			t.Fatal("target never became Ready")
		}

		if err := h.manager.Close(); err != nil {
			t.Fatalf("Manager.Close: %v", err)
		}

		// The auto-attached session connection must be closed and the
		// browser asked to detach.
		select {
		case <-child.Context().Done():
		case <-time.After(5 * time.Second):
			t.Fatal("child session was not closed with the Manager")
		}
		h.fake.waitFor(t, "detach on close", func(c call) bool {
			return c.method == "Target.detachFromTarget"
		})

		// The error channel must terminate instead of blocking forever.
		select {
		case _, ok := <-h.manager.Err():
			if ok {
				t.Fatal("Manager.Err channel was not closed")
			}
		case <-time.After(5 * time.Second):
			t.Fatal("Manager.Err channel still open after Close")
		}
	})
}
