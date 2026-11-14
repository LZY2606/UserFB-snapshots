package selenium

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/tebeka/selenium/task_support/taskcheck"
)

// wireMessage is a BiDi message on the test WebSocket connection, used in
// both directions.
type wireMessage struct {
	ID         int             `json:"id,omitempty"`
	Type       string          `json:"type,omitempty"`
	Method     string          `json:"method,omitempty"`
	Params     json.RawMessage `json:"params,omitempty"`
	Result     json.RawMessage `json:"result,omitempty"`
	Error      string          `json:"error,omitempty"`
	Message    string          `json:"message,omitempty"`
	Stacktrace string          `json:"stacktrace,omitempty"`
}

// bidiPeer is the test-controlled remote end of a BiDi WebSocket connection.
// Commands sent by the client are delivered on commands; messages queued on
// outgoing are written to the client. Ordering is arranged by the test
// through channel handshakes.
type bidiPeer struct {
	conn     *websocket.Conn
	commands chan wireMessage
	outgoing chan wireMessage
	closed   chan struct{}
}

func newBidiPeer(conn *websocket.Conn) *bidiPeer {
	p := &bidiPeer{
		conn:     conn,
		commands: make(chan wireMessage, 32),
		outgoing: make(chan wireMessage, 32),
		closed:   make(chan struct{}),
	}
	go p.readLoop()
	go p.writeLoop()
	return p
}

func (p *bidiPeer) readLoop() {
	defer close(p.closed)
	for {
		_, data, err := p.conn.ReadMessage()
		if err != nil {
			return
		}
		var m wireMessage
		if err := json.Unmarshal(data, &m); err != nil {
			return
		}
		p.commands <- m
	}
}

func (p *bidiPeer) writeLoop() {
	for m := range p.outgoing {
		if err := p.conn.WriteJSON(m); err != nil {
			return
		}
	}
}

// next waits for the next command sent by the client.
func (p *bidiPeer) next(t *testing.T) wireMessage {
	t.Helper()
	select {
	case m := <-p.commands:
		return m
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for a BiDi command from the client")
		return wireMessage{}
	}
}

// noCommand fails if the client has sent a command that was not consumed.
func (p *bidiPeer) noCommand(t *testing.T) {
	t.Helper()
	select {
	case m := <-p.commands:
		t.Fatalf("unexpected BiDi command from the client: %+v", m)
	default:
	}
}

// bidiFixture is a local WebDriver HTTP endpoint with a BiDi WebSocket
// upgrade path.
type bidiFixture struct {
	server *httptest.Server
	peerCh chan *bidiPeer

	mu          sync.Mutex
	endpoint    string
	alwaysMatch map[string]interface{}
	unexpected  []string
}

func newBiDiFixture(t *testing.T) *bidiFixture {
	t.Helper()
	f := &bidiFixture{peerCh: make(chan *bidiPeer, 1)}
	f.server = httptest.NewServer(http.HandlerFunc(f.serve))
	f.endpoint = "ws" + strings.TrimPrefix(f.server.URL, "http") + "/bidi"
	t.Cleanup(f.server.Close)
	return f
}

func (f *bidiFixture) serve(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	switch {
	case r.Method == "POST" && r.URL.Path == "/session":
		var body struct {
			Capabilities struct {
				AlwaysMatch map[string]interface{} `json:"alwaysMatch"`
			} `json:"capabilities"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		f.mu.Lock()
		f.alwaysMatch = body.Capabilities.AlwaysMatch
		endpoint := f.endpoint
		f.mu.Unlock()
		caps := map[string]interface{}{"browserName": "firefox"}
		if endpoint != "" {
			caps["webSocketUrl"] = endpoint
		}
		json.NewEncoder(w).Encode(map[string]interface{}{
			"value": map[string]interface{}{
				"sessionId":    "session-1",
				"capabilities": caps,
			},
		})
	case r.URL.Path == "/bidi":
		conn, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			return
		}
		p := newBidiPeer(conn)
		f.peerCh <- p
		<-p.closed
		conn.Close()
	case r.Method == "DELETE" && r.URL.Path == "/session/session-1":
		json.NewEncoder(w).Encode(map[string]interface{}{"value": nil})
	default:
		f.mu.Lock()
		f.unexpected = append(f.unexpected, r.Method+" "+r.URL.Path)
		f.mu.Unlock()
		w.WriteHeader(http.StatusNotFound)
		json.NewEncoder(w).Encode(map[string]interface{}{
			"value": map[string]interface{}{
				"error":   "unknown command",
				"message": "fixture accepts no capability re-fetch",
			},
		})
	}
}

func (f *bidiFixture) setEndpoint(endpoint string) {
	f.mu.Lock()
	f.endpoint = endpoint
	f.mu.Unlock()
}

func (f *bidiFixture) getAlwaysMatch() map[string]interface{} {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.alwaysMatch
}

func (f *bidiFixture) unexpectedRequests() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return append([]string(nil), f.unexpected...)
}

func (f *bidiFixture) newRemote(t *testing.T, caps Capabilities) WebDriver {
	t.Helper()
	wd, err := NewRemote(caps, f.server.URL)
	if err != nil {
		t.Fatalf("NewRemote: %v", err)
	}
	return wd
}

// peer waits for the client to open the WebSocket connection.
func (f *bidiFixture) peer(t *testing.T) *bidiPeer {
	t.Helper()
	select {
	case p := <-f.peerCh:
		return p
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for the BiDi WebSocket connection")
		return nil
	}
}

type openResult struct {
	client *BiDiClient
	err    error
}

func asyncOpenBiDi(wd WebDriver) chan openResult {
	ch := make(chan openResult, 1)
	go func() {
		c, err := OpenBiDi(context.Background(), wd)
		ch <- openResult{c, err}
	}()
	return ch
}

// openBiDi runs OpenBiDi against the fixture, confirms the subscription and
// returns the client, the peer and the session.subscribe command.
func openBiDi(t *testing.T, f *bidiFixture, wd WebDriver) (*BiDiClient, *bidiPeer, wireMessage) {
	t.Helper()
	resCh := asyncOpenBiDi(wd)
	p := f.peer(t)
	sub := p.next(t)
	p.outgoing <- wireMessage{ID: sub.ID, Type: "success", Result: json.RawMessage(`{}`)}
	res := <-resCh
	if res.err != nil {
		t.Fatalf("OpenBiDi: %v", res.err)
	}
	return res.client, p, sub
}

func parseParams(t *testing.T, m wireMessage) map[string]interface{} {
	t.Helper()
	var params map[string]interface{}
	if err := json.Unmarshal(m.Params, &params); err != nil {
		t.Fatalf("command %s has invalid params: %v", m.Method, err)
	}
	return params
}

type evalResult struct {
	obj *BiDiObject
	err error
}

func asyncEvaluate(c *BiDiClient, contextID, expression string) chan evalResult {
	ch := make(chan evalResult, 1)
	go func() {
		o, err := c.EvaluateObject(context.Background(), contextID, expression)
		ch <- evalResult{o, err}
	}()
	return ch
}

// evaluateObject drives one successful EvaluateObject round trip.
func evaluateObject(t *testing.T, c *BiDiClient, p *bidiPeer, contextID, expression, realm, handle string) *BiDiObject {
	t.Helper()
	ch := asyncEvaluate(c, contextID, expression)
	m := p.next(t)
	if m.Method != "script.evaluate" {
		t.Fatalf("expected script.evaluate, got %s", m.Method)
	}
	result := fmt.Sprintf(`{"type":"success","realm":%q,"result":{"type":"object","handle":%q}}`, realm, handle)
	p.outgoing <- wireMessage{ID: m.ID, Type: "success", Result: json.RawMessage(result)}
	res := <-ch
	if res.err != nil {
		t.Fatalf("EvaluateObject: %v", res.err)
	}
	return res.obj
}

type callResult struct {
	raw json.RawMessage
	err error
}

func asyncCall(o *BiDiObject, declaration string, args []json.RawMessage) chan callResult {
	ch := make(chan callResult, 1)
	go func() {
		raw, err := o.Call(context.Background(), declaration, args)
		ch <- callResult{raw, err}
	}()
	return ch
}

func asyncCallCtx(ctx context.Context, o *BiDiObject, declaration string) chan callResult {
	ch := make(chan callResult, 1)
	go func() {
		raw, err := o.Call(ctx, declaration, nil)
		ch <- callResult{raw, err}
	}()
	return ch
}

type releaseResult struct {
	err error
}

func asyncRelease(o *BiDiObject) chan releaseResult {
	ch := make(chan releaseResult, 1)
	go func() {
		ch <- releaseResult{o.Release(context.Background())}
	}()
	return ch
}

func TestBiDiNegotiation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("always_match_carries_websocket_url", func(t *testing.T) {
			f := newBiDiFixture(t)
			caps := Capabilities{"browserName": "firefox", "webSocketUrl": true}
			wd := f.newRemote(t, caps)
			defer wd.Quit()

			am := f.getAlwaysMatch()
			c.Check(am["webSocketUrl"] == true,
				"alwaysMatch should carry webSocketUrl:true, got %v", am)
			c.Check(am["browserName"] == "firefox",
				"alwaysMatch should keep browserName, got %v", am)
			_, mutated := caps["alwaysMatch"]
			c.Check(!mutated, "input capabilities map must not be modified: %v", caps)

			client, _, sub := openBiDi(t, f, wd)
			defer client.Close()
			c.Check(sub.Method == "session.subscribe",
				"first BiDi command should be session.subscribe, got %q", sub.Method)
			params := parseParams(t, sub)
			events, ok := params["events"].([]interface{})
			c.Check(ok && len(events) == 1 && events[0] == "script.realmDestroyed",
				"subscribe events should be [script.realmDestroyed], got %v", params["events"])
			c.Check(len(f.unexpectedRequests()) == 0,
				"OpenBiDi must not re-fetch capabilities, unexpected requests: %v",
				f.unexpectedRequests())
		})

		t.Run("missing_endpoint", func(t *testing.T) {
			f := newBiDiFixture(t)
			f.setEndpoint("")
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			_, err := OpenBiDi(context.Background(), wd)
			c.Check(errors.Is(err, ErrBiDiUnavailable),
				"missing endpoint should yield ErrBiDiUnavailable, got %v", err)
		})

		t.Run("non_ws_endpoint", func(t *testing.T) {
			f := newBiDiFixture(t)
			f.setEndpoint("http://127.0.0.1:1/bidi")
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			_, err := OpenBiDi(context.Background(), wd)
			c.Check(errors.Is(err, ErrBiDiUnavailable),
				"non-ws endpoint should yield ErrBiDiUnavailable, got %v", err)
		})

		t.Run("unsupported_driver", func(t *testing.T) {
			_, err := OpenBiDi(context.Background(), nil)
			c.Check(errors.Is(err, ErrBiDiUnavailable),
				"unsupported WebDriver should yield ErrBiDiUnavailable, got %v", err)
		})

		t.Run("event_before_subscribe_confirm", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()

			resCh := asyncOpenBiDi(wd)
			p := f.peer(t)
			sub := p.next(t)
			// The destruction event arrives before the subscription is
			// confirmed; it must still be recorded.
			p.outgoing <- wireMessage{
				Type:   "event",
				Method: "script.realmDestroyed",
				Params: json.RawMessage(`{"realm":"r-early"}`),
			}
			p.outgoing <- wireMessage{ID: sub.ID, Type: "success", Result: json.RawMessage(`{}`)}
			res := <-resCh
			c.Check(res.err == nil, "OpenBiDi should succeed, got %v", res.err)
			if res.err != nil {
				return
			}
			defer res.client.Close()

			// An evaluate resolving into the already-destroyed realm must
			// not publish an object.
			ech := asyncEvaluate(res.client, "ctx-1", "document.forms[0]")
			m := p.next(t)
			c.Check(m.Method == "script.evaluate", "expected script.evaluate, got %q", m.Method)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-early","result":{"type":"object","handle":"h-1"}}`),
			}
			er := <-ech
			c.Check(errors.Is(er.err, ErrBiDiRealmDestroyed),
				"evaluate into destroyed realm should fail, got %v", er.err)
			c.Check(er.obj == nil, "no object may be published for a destroyed realm")
		})
	})
}

func TestBiDiObjects(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("evaluate_call_and_raw_values", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()

			ech := asyncEvaluate(client, "ctx-1", "document.forms.order")
			m := p.next(t)
			c.Check(m.Method == "script.evaluate", "expected script.evaluate, got %q", m.Method)
			params := parseParams(t, m)
			c.Check(params["expression"] == "document.forms.order",
				"evaluate expression mismatch: %v", params["expression"])
			target, _ := params["target"].(map[string]interface{})
			c.Check(target["context"] == "ctx-1",
				"evaluate target should be the browsing context, got %v", params["target"])
			c.Check(params["awaitPromise"] == true, "awaitPromise should be true")
			c.Check(params["resultOwnership"] == "root",
				"evaluate should request root ownership, got %v", params["resultOwnership"])
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"object","handle":"h-1","value":[["name",{"type":"string","value":"order"}]]}}`),
			}
			er := <-ech
			if er.err != nil {
				t.Fatalf("EvaluateObject: %v", er.err)
			}
			obj := er.obj
			c.Check(obj.Realm() == "r-1", "object realm should be r-1, got %q", obj.Realm())
			c.Check(obj.Handle() == "h-1", "object handle should be h-1, got %q", obj.Handle())

			arg := json.RawMessage(`{"type":"string","value":"qty"}`)
			cch := asyncCall(obj, "function(field){ return this.elements[field].value; }", []json.RawMessage{arg})
			m = p.next(t)
			c.Check(m.Method == "script.callFunction", "expected script.callFunction, got %q", m.Method)
			params = parseParams(t, m)
			c.Check(params["functionDeclaration"] == "function(field){ return this.elements[field].value; }",
				"functionDeclaration mismatch: %v", params["functionDeclaration"])
			target, _ = params["target"].(map[string]interface{})
			c.Check(target["realm"] == "r-1",
				"call target should be the object realm, got %v", params["target"])
			thisArg, _ := params["this"].(map[string]interface{})
			c.Check(thisArg["handle"] == "h-1",
				"call this should be the object handle, got %v", params["this"])
			args, _ := params["arguments"].([]interface{})
			c.Check(len(args) == 1, "call should pass one argument, got %v", params["arguments"])
			if len(args) == 1 {
				raw, _ := json.Marshal(args[0])
				c.Check(string(raw) == string(arg),
					"argument LocalValue should pass through unchanged, got %s", raw)
			}
			c.Check(params["resultOwnership"] == "none",
				"call should not retain ownership, got %v", params["resultOwnership"])
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"bigint","value":"9007199254740993"}}`),
			}
			cr := <-cch
			c.Check(cr.err == nil, "Call should succeed, got %v", cr.err)
			c.Check(string(cr.raw) == `{"type":"bigint","value":"9007199254740993"}`,
				"bigint RemoteValue must stay raw, got %s", cr.raw)

			// Non-finite numbers keep their tagged form.
			cch = asyncCall(obj, "function(){ return this.ratio; }", nil)
			m = p.next(t)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"number","value":"NaN"}}`),
			}
			cr = <-cch
			c.Check(cr.err == nil, "Call should succeed, got %v", cr.err)
			c.Check(string(cr.raw) == `{"type":"number","value":"NaN"}`,
				"NaN RemoteValue must stay raw, got %s", cr.raw)

			// Node values keep sharedId and handle.
			cch = asyncCall(obj, "function(){ return this.submitButton; }", nil)
			m = p.next(t)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"node","sharedId":"sh-1","handle":"h-2","value":{"nodeType":1,"localName":"button"}}}`),
			}
			cr = <-cch
			c.Check(cr.err == nil, "Call should succeed, got %v", cr.err)
			c.Check(string(cr.raw) == `{"type":"node","sharedId":"sh-1","handle":"h-2","value":{"nodeType":1,"localName":"button"}}`,
				"node RemoteValue must stay raw, got %s", cr.raw)
		})

		t.Run("out_of_order_completion", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()

			obj1 := evaluateObject(t, client, p, "ctx-1", "window.one", "r-1", "h-1")
			obj2 := evaluateObject(t, client, p, "ctx-1", "window.two", "r-1", "h-2")

			first := asyncCall(obj1, "returnOne", nil)
			second := asyncCall(obj2, "returnTwo", nil)

			// Collect both commands; either may arrive first on the wire.
			cmds := map[string]wireMessage{}
			for len(cmds) < 2 {
				m := p.next(t)
				c.Check(m.Method == "script.callFunction",
					"expected script.callFunction, got %q", m.Method)
				params := parseParams(t, m)
				decl, _ := params["functionDeclaration"].(string)
				cmds[decl] = m
			}
			// Reply in reverse order: the second object settles first.
			p.outgoing <- wireMessage{
				ID:     cmds["returnTwo"].ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"string","value":"two"}}`),
			}
			r2 := <-second
			c.Check(r2.err == nil && string(r2.raw) == `{"type":"string","value":"two"}`,
				"second object should get its own reply, got %s, %v", r2.raw, r2.err)
			p.outgoing <- wireMessage{
				ID:     cmds["returnOne"].ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"string","value":"one"}}`),
			}
			r1 := <-first
			c.Check(r1.err == nil && string(r1.raw) == `{"type":"string","value":"one"}`,
				"first object should get its own reply, got %s, %v", r1.raw, r1.err)
		})

		t.Run("script_exception_and_protocol_error", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()
			obj := evaluateObject(t, client, p, "ctx-1", "window.form", "r-1", "h-1")

			// Script exception inside a top-level success reply.
			cch := asyncCall(obj, "function(){ this.missing(); }", nil)
			m := p.next(t)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"exception","realm":"r-1","exceptionDetails":{"text":"TypeError: this.missing is not a function","columnNumber":12,"exception":{"type":"error","value":{"message":"boom"}}}}`),
			}
			cr := <-cch
			var scriptErr *BiDiScriptError
			c.Check(errors.As(cr.err, &scriptErr),
				"exception result should yield *BiDiScriptError, got %T %v", cr.err, cr.err)
			if scriptErr != nil {
				c.Check(scriptErr.Realm == "r-1",
					"script error realm should be r-1, got %q", scriptErr.Realm)
				c.Check(strings.Contains(string(scriptErr.Details), "this.missing is not a function"),
					"script error should keep exceptionDetails, got %s", scriptErr.Details)
			}

			// Top-level protocol error.
			cch = asyncCall(obj, "function(){ return 1; }", nil)
			m = p.next(t)
			p.outgoing <- wireMessage{
				ID:         m.ID,
				Type:       "error",
				Error:      "invalid argument",
				Message:    "handle is not valid",
				Stacktrace: "root/frames",
			}
			cr = <-cch
			var protoErr *BiDiProtocolError
			c.Check(errors.As(cr.err, &protoErr),
				"protocol error should yield *BiDiProtocolError, got %T %v", cr.err, cr.err)
			if protoErr != nil {
				c.Check(protoErr.Code == "invalid argument" && protoErr.Message == "handle is not valid",
					"protocol error fields mismatch: %+v", protoErr)
			}

			// A plain result object containing an "error" property is not
			// an error.
			cch = asyncCall(obj, "function(){ return {error: 0}; }", nil)
			m = p.next(t)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"object","value":[["error",{"type":"number","value":0}]]}}`),
			}
			cr = <-cch
			c.Check(cr.err == nil,
				"result object with an error property must not be misjudged: %v", cr.err)
			c.Check(string(cr.raw) == `{"type":"object","value":[["error",{"type":"number","value":0}]]}`,
				"result object should pass through raw, got %s", cr.raw)
		})

		t.Run("release_failure_then_success", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()
			obj := evaluateObject(t, client, p, "ctx-1", "window.form", "r-1", "h-1")

			// The first release is rejected by the protocol.
			rch := asyncRelease(obj)
			m := p.next(t)
			c.Check(m.Method == "script.disown", "expected script.disown, got %q", m.Method)
			params := parseParams(t, m)
			target, _ := params["target"].(map[string]interface{})
			c.Check(target["realm"] == "r-1",
				"disown target should be the object realm, got %v", params["target"])
			handles, _ := params["handles"].([]interface{})
			c.Check(len(handles) == 1 && handles[0] == "h-1",
				"disown handles should contain only the object handle, got %v", params["handles"])
			p.outgoing <- wireMessage{
				ID:      m.ID,
				Type:    "error",
				Error:   "unknown error",
				Message: "cannot disown now",
			}
			rr := <-rch
			var protoErr *BiDiProtocolError
			c.Check(errors.As(rr.err, &protoErr),
				"rejected release should surface the protocol error, got %T %v", rr.err, rr.err)

			// The object is still usable after the rejected release.
			cch := asyncCall(obj, "function(){ return 7; }", nil)
			m = p.next(t)
			c.Check(m.Method == "script.callFunction",
				"object should still be callable after a rejected release, got %q", m.Method)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"number","value":7}}`),
			}
			cr := <-cch
			c.Check(cr.err == nil && string(cr.raw) == `{"type":"number","value":7}`,
				"call after rejected release should work, got %s, %v", cr.raw, cr.err)

			// Retried release succeeds.
			rch = asyncRelease(obj)
			m = p.next(t)
			c.Check(m.Method == "script.disown", "expected retried script.disown, got %q", m.Method)
			p.outgoing <- wireMessage{ID: m.ID, Type: "success", Result: json.RawMessage(`{}`)}
			rr = <-rch
			c.Check(rr.err == nil, "retried release should succeed, got %v", rr.err)

			// Repeated release is a local no-op.
			c.Check(obj.Release(context.Background()) == nil,
				"repeated release should return nil")
			p.noCommand(t)

			// Calls after a successful release fail locally.
			_, err := obj.Call(context.Background(), "function(){ return 1; }", nil)
			c.Check(errors.Is(err, ErrBiDiObjectReleased),
				"call after release should yield ErrBiDiObjectReleased, got %v", err)
			p.noCommand(t)
		})

		t.Run("evaluate_without_handle", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()

			ech := asyncEvaluate(client, "ctx-1", "6 * 7")
			m := p.next(t)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"number","value":42}}`),
			}
			er := <-ech
			c.Check(errors.Is(er.err, ErrBiDiNotObject),
				"primitive result should yield ErrBiDiNotObject, got %v", er.err)
			c.Check(er.obj == nil, "no object may be published without a handle")
		})
	})
}

func TestBiDiRealmLifetime(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		t.Run("in_flight_call_destroyed", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()

			obj1 := evaluateObject(t, client, p, "ctx-1", "window.one", "r-1", "h-1")
			obj2 := evaluateObject(t, client, p, "ctx-2", "window.two", "r-2", "h-2")

			// A call in flight when its realm is destroyed fails.
			cch := asyncCall(obj1, "function(){ return this.step(); }", nil)
			m := p.next(t)
			c.Check(m.Method == "script.callFunction", "expected script.callFunction, got %q", m.Method)
			p.outgoing <- wireMessage{
				Type:   "event",
				Method: "script.realmDestroyed",
				Params: json.RawMessage(`{"realm":"r-1"}`),
			}
			cr := <-cch
			c.Check(errors.Is(cr.err, ErrBiDiRealmDestroyed),
				"in-flight call should to a destroyed realm should fail, got %v", cr.err)

			// The object no longer sends commands.
			_, err := obj1.Call(context.Background(), "function(){ return 1; }", nil)
			c.Check(errors.Is(err, ErrBiDiRealmDestroyed),
				"call on a destroyed realm object should fail locally, got %v", err)
			err = obj1.Release(context.Background())
			c.Check(errors.Is(err, ErrBiDiRealmDestroyed),
				"release on a destroyed realm object should fail locally, got %v", err)
			p.noCommand(t)

			// The sibling realm keeps working.
			cch = asyncCall(obj2, "function(){ return 2; }", nil)
			m = p.next(t)
			c.Check(m.Method == "script.callFunction",
				"sibling realm should still accept commands, got %q", m.Method)
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-2","result":{"type":"number","value":2}}`),
			}
			cr = <-cch
			c.Check(cr.err == nil && string(cr.raw) == `{"type":"number","value":2}`,
				"sibling realm call should succeed, got %s, %v", cr.raw, cr.err)
		})

		t.Run("late_evaluate_reply", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()

			// The destruction event for the target realm arrives before the
			// successful evaluate reply: the object must not be published.
			ech := asyncEvaluate(client, "ctx-9", "document.forms[0]")
			m := p.next(t)
			c.Check(m.Method == "script.evaluate", "expected script.evaluate, got %q", m.Method)
			p.outgoing <- wireMessage{
				Type:   "event",
				Method: "script.realmDestroyed",
				Params: json.RawMessage(`{"realm":"r-late"}`),
			}
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-late","result":{"type":"object","handle":"h-9"}}`),
			}
			er := <-ech
			c.Check(errors.Is(er.err, ErrBiDiRealmDestroyed),
				"late evaluate reply for a destroyed realm should fail, got %v", er.err)
			c.Check(er.obj == nil, "the stale object must not be republished")
		})

		t.Run("ctx_cancel", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			defer wd.Quit()
			client, p, _ := openBiDi(t, f, wd)
			defer client.Close()
			obj := evaluateObject(t, client, p, "ctx-1", "window.form", "r-1", "h-1")

			ctx, cancel := context.WithCancel(context.Background())
			cch := asyncCallCtx(ctx, obj, "function(){ return this.slow(); }")
			m := p.next(t)
			c.Check(m.Method == "script.callFunction", "expected script.callFunction, got %q", m.Method)
			cancel()
			cr := <-cch
			c.Check(errors.Is(cr.err, context.Canceled),
				"cancelled call should end with the context error, got %v", cr.err)

			// The late reply is ignored and does not settle anything.
			p.outgoing <- wireMessage{
				ID:     m.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"number","value":1}}`),
			}

			// The object remains usable and the cancelled id is not reused.
			cch = asyncCall(obj, "function(){ return 2; }", nil)
			m2 := p.next(t)
			c.Check(m2.ID != m.ID, "command ids must not be reused, got %d twice", m2.ID)
			p.outgoing <- wireMessage{
				ID:     m2.ID,
				Type:   "success",
				Result: json.RawMessage(`{"type":"success","realm":"r-1","result":{"type":"number","value":2}}`),
			}
			cr = <-cch
			c.Check(cr.err == nil && string(cr.raw) == `{"type":"number","value":2}`,
				"object should stay usable after a cancelled call, got %s, %v", cr.raw, cr.err)
		})

		t.Run("close_ends_waits", func(t *testing.T) {
			f := newBiDiFixture(t)
			wd := f.newRemote(t, Capabilities{"browserName": "firefox", "webSocketUrl": true})
			client, p, _ := openBiDi(t, f, wd)
			obj := evaluateObject(t, client, p, "ctx-1", "window.form", "r-1", "h-1")

			cch := asyncCall(obj, "function(){ return this.slow(); }", nil)
			m := p.next(t)
			c.Check(m.Method == "script.callFunction", "expected script.callFunction, got %q", m.Method)
			c.Check(client.Close() == nil, "Close should succeed")
			cr := <-cch
			c.Check(errors.Is(cr.err, ErrBiDiClosed),
				"in-flight call should end with ErrBiDiClosed, got %v", cr.err)

			_, err := obj.Call(context.Background(), "function(){ return 1; }", nil)
			c.Check(errors.Is(err, ErrBiDiClosed),
				"call after Close should yield ErrBiDiClosed, got %v", err)
			err = obj.Release(context.Background())
			c.Check(errors.Is(err, ErrBiDiClosed),
				"release after Close should yield ErrBiDiClosed, got %v", err)
			_, err = client.EvaluateObject(context.Background(), "ctx-1", "1")
			c.Check(errors.Is(err, ErrBiDiClosed),
				"evaluate after Close should yield ErrBiDiClosed, got %v", err)
			c.Check(client.Close() == nil, "Close should be idempotent")

			// Close neither ends the WebDriver session nor sends
			// side-effecting commands.
			p.noCommand(t)
			c.Check(wd.Quit() == nil, "WebDriver session should survive BiDi Close")
		})
	})
}
