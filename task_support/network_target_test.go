package selenium_test

// Regression tests for the WebDriver BiDi network interception API in
// network_bidi.go. A local Gorilla WebSocket peer plays the remote end; all
// synchronization uses channel handshakes, never sleeps.

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/tebeka/selenium"
	"github.com/tebeka/selenium/task_support/taskcheck"
)

// peerCmd is one BiDi command frame received by the local peer.
type peerCmd struct {
	id     uint64
	method string
	params json.RawMessage
	raw    string
}

// bidiPeer is the server side of the BiDi WebSocket connection.
type bidiPeer struct {
	t    *testing.T
	conn *websocket.Conn
	cmds chan peerCmd
}

func (p *bidiPeer) readLoop() {
	for {
		_, data, err := p.conn.ReadMessage()
		if err != nil {
			close(p.cmds)
			return
		}
		var env struct {
			ID     uint64          `json:"id"`
			Method string          `json:"method"`
			Params json.RawMessage `json:"params"`
		}
		if err := json.Unmarshal(data, &env); err != nil {
			continue
		}
		p.cmds <- peerCmd{id: env.ID, method: env.Method, params: env.Params, raw: string(data)}
	}
}

// next waits for the next client command.
func (p *bidiPeer) next(t *testing.T) peerCmd {
	t.Helper()
	select {
	case c, ok := <-p.cmds:
		if !ok {
			t.Fatal("bidi peer connection closed while waiting for a command")
		}
		return c
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for a bidi command")
		return peerCmd{}
	}
}

func (p *bidiPeer) send(s string) {
	if err := p.conn.WriteMessage(websocket.TextMessage, []byte(s)); err != nil {
		p.t.Errorf("peer send: %v", err)
	}
}

func (p *bidiPeer) reply(id uint64, result string) {
	p.send(fmt.Sprintf(`{"type":"success","id":%d,"result":%s}`, id, result))
}

func (p *bidiPeer) replyError(id uint64, code, message string) {
	c, _ := json.Marshal(code)
	m, _ := json.Marshal(message)
	p.send(fmt.Sprintf(`{"type":"error","id":%d,"error":%s,"message":%s}`, id, c, m))
}

// harness is a local WebDriver HTTP endpoint plus a BiDi WebSocket peer.
type harness struct {
	t           *testing.T
	server      *httptest.Server
	negotiateWS atomic.Bool
	matchCh     chan map[string]interface{}
	peerCh      chan *bidiPeer
	peer        *bidiPeer
}

func newHarness(t *testing.T) *harness {
	t.Helper()
	h := &harness{
		t:       t,
		matchCh: make(chan map[string]interface{}, 4),
		peerCh:  make(chan *bidiPeer, 1),
	}
	upgrader := websocket.Upgrader{}
	h.server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/bidi" {
			conn, err := upgrader.Upgrade(w, r, nil)
			if err != nil {
				return
			}
			p := &bidiPeer{t: t, conn: conn, cmds: make(chan peerCmd, 64)}
			h.peerCh <- p
			p.readLoop()
			return
		}
		w.Header().Set("Content-Type", "application/json")
		switch r.Method + " " + r.URL.Path {
		case "POST /session":
			var body struct {
				Capabilities struct {
					AlwaysMatch map[string]interface{} `json:"alwaysMatch"`
				} `json:"capabilities"`
			}
			if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
				t.Errorf("decode session request: %v", err)
			}
			h.matchCh <- body.Capabilities.AlwaysMatch
			caps := `"browserName":"firefox"`
			if h.negotiateWS.Load() {
				wsURL := "ws" + strings.TrimPrefix(h.server.URL, "http") + "/bidi"
				caps += fmt.Sprintf(`,"webSocketUrl":%q`, wsURL)
			}
			fmt.Fprintf(w, `{"value":{"sessionId":"form","capabilities":{%s}}}`, caps)
		case "GET /session/form/title":
			fmt.Fprint(w, `{"value":"order form"}`)
		case "DELETE /session/form":
			fmt.Fprint(w, `{"value":null}`)
		default:
			t.Errorf("unexpected HTTP %s %s", r.Method, r.URL.Path)
			http.Error(w, `{"value":{"error":"unknown command","message":"unknown command"}}`, 404)
		}
	}))
	return h
}

// newSession creates a WebDriver session and checks the negotiated
// capabilities on the wire.
func (h *harness) newSession(t *testing.T, c *taskcheck.C, caps selenium.Capabilities) selenium.WebDriver {
	t.Helper()
	wd, err := selenium.NewRemote(caps, h.server.URL)
	c.Check(err == nil, "NewRemote: %v", err)
	if err != nil {
		t.FailNow()
	}
	return wd
}

// openNetwork drives OpenNetwork against the peer: it answers the
// session.subscribe command and returns the opened Network.
func (h *harness) openNetwork(t *testing.T, c *taskcheck.C, wd selenium.WebDriver) *selenium.Network {
	t.Helper()
	type result struct {
		n   *selenium.Network
		err error
	}
	resCh := make(chan result, 1)
	go func() {
		n, err := selenium.OpenNetwork(wd)
		resCh <- result{n, err}
	}()
	select {
	case p := <-h.peerCh:
		h.peer = p
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for the bidi connection")
	}
	cmd := h.peer.next(t)
	c.Check(cmd.method == "session.subscribe", "first bidi command = %q, want session.subscribe", cmd.method)
	var params struct {
		Events []string `json:"events"`
	}
	if err := json.Unmarshal(cmd.params, &params); err != nil {
		t.Fatalf("decode subscribe params: %v", err)
	}
	c.Check(strings.Join(params.Events, ",") == "network.beforeRequestSent,network.responseStarted",
		"subscribed events = %v", params.Events)
	h.peer.reply(cmd.id, `{}`)
	select {
	case res := <-resCh:
		c.Check(res.err == nil, "OpenNetwork: %v", res.err)
		if res.err != nil {
			t.FailNow()
		}
		return res.n
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for OpenNetwork")
		return nil
	}
}

// addIntercept runs Add, checks the command on the wire and replies with the
// given result JSON.
func (h *harness) addIntercept(t *testing.T, c *taskcheck.C, n *selenium.Network, phases, patterns []string, result string) (*selenium.NetworkIntercept, peerCmd) {
	t.Helper()
	type aresult struct {
		i   *selenium.NetworkIntercept
		err error
	}
	resCh := make(chan aresult, 1)
	go func() {
		i, err := n.Add(phases, patterns)
		resCh <- aresult{i, err}
	}()
	cmd := h.peer.next(t)
	c.Check(cmd.method == "network.addIntercept", "command = %q, want network.addIntercept", cmd.method)
	h.peer.reply(cmd.id, result)
	select {
	case res := <-resCh:
		c.Check(res.err == nil, "Add: %v", res.err)
		if res.err != nil {
			t.FailNow()
		}
		return res.i, cmd
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for Add")
		return nil, peerCmd{}
	}
}

// blockedEvent builds a network event frame. extra is appended to the params
// object verbatim, e.g. `,"extra":"keep"`.
func blockedEvent(method, request string, redirectCount int, intercepts []string, extra string) string {
	ic, _ := json.Marshal(intercepts)
	return fmt.Sprintf(`{"type":"event","method":%q,"params":{"context":"tab","isBlocked":true,"redirectCount":%d,"intercepts":%s%s,"request":{"request":%q,"url":"https://form.invalid/send","method":"POST","headers":[]}}}`,
		method, redirectCount, ic, extra, request)
}

// responseEvent builds a network.responseStarted event frame.
func responseEvent(request string, redirectCount int, intercepts []string) string {
	ic, _ := json.Marshal(intercepts)
	return fmt.Sprintf(`{"type":"event","method":"network.responseStarted","params":{"context":"tab","isBlocked":true,"redirectCount":%d,"intercepts":%s,"request":{"request":%q,"url":"https://form.invalid/send","method":"POST","headers":[]},"response":{"url":"https://form.invalid/send","status":200,"headers":[]}}}`,
		redirectCount, ic, request)
}

// awaitErr waits for one decision result.
func awaitErr(t *testing.T, ch <-chan error) error {
	t.Helper()
	select {
	case err := <-ch:
		return err
	case <-time.After(15 * time.Second):
		t.Fatal("timed out waiting for a decision result")
		return nil
	}
}

func TestNetworkRegistration(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newHarness(t)
		defer h.server.Close()
		h.negotiateWS.Store(true)

		wd := h.newSession(t, c, selenium.Capabilities{"browserName": "firefox", "webSocketUrl": true})
		am := <-h.matchCh
		c.Check(am["webSocketUrl"] == true, "alwaysMatch webSocketUrl = %v, want true", am["webSocketUrl"])
		c.Check(am["browserName"] == "firefox", "alwaysMatch browserName = %v", am["browserName"])

		n := h.openNetwork(t, c, wd)

		// Register a rule covering both phases with a URL pattern.
		phases := []string{"beforeRequestSent", "responseStarted"}
		patterns := []string{"https://form.invalid/send"}
		ruleA, cmd := h.addIntercept(t, c, n, phases, patterns, `{"intercept":"A"}`)
		var addParams struct {
			Phases      []string `json:"phases"`
			URLPatterns []struct {
				Type    string `json:"type"`
				Pattern string `json:"pattern"`
			} `json:"urlPatterns"`
		}
		if err := json.Unmarshal(cmd.params, &addParams); err != nil {
			t.Fatalf("decode addIntercept params: %v", err)
		}
		c.Check(strings.Join(addParams.Phases, ",") == "beforeRequestSent,responseStarted",
			"addIntercept phases = %v", addParams.Phases)
		c.Check(len(addParams.URLPatterns) == 1 && addParams.URLPatterns[0].Type == "string" &&
			addParams.URLPatterns[0].Pattern == "https://form.invalid/send",
			"addIntercept urlPatterns = %+v", addParams.URLPatterns)
		c.Check(ruleA.ID() == "A", "intercept ID = %q, want A", ruleA.ID())

		// Mutating the caller slices afterwards must not corrupt the client.
		phases[0] = "mutated"
		patterns[0] = "mutated"

		// A rule without URL patterns omits the urlPatterns member.
		ruleB, cmdB := h.addIntercept(t, c, n, []string{"beforeRequestSent"}, nil, `{"intercept":"B"}`)
		var rawParams map[string]json.RawMessage
		if err := json.Unmarshal(cmdB.params, &rawParams); err != nil {
			t.Fatalf("decode addIntercept params: %v", err)
		}
		_, hasPatterns := rawParams["urlPatterns"]
		c.Check(!hasPatterns, "urlPatterns should be omitted, params = %s", cmdB.params)
		c.Check(ruleB.ID() == "B", "intercept ID = %q, want B", ruleB.ID())

		// Invalid registrations fail locally, without sending a frame.
		_, err := n.Add(nil, nil)
		c.Check(err != nil, "Add with empty phases should fail")
		_, err = n.Add([]string{"responseCompleted"}, nil)
		c.Check(err != nil, "Add with an unknown phase should fail")

		// Removing ruleA sends exactly one removeIntercept command. The
		// failed Adds above and the idempotent re-removal below send
		// nothing, so the next two commands on the wire must be the
		// removals of A and B, in this order.
		errCh := make(chan error, 2)
		go func() { errCh <- ruleA.Remove() }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.removeIntercept", "command = %q, want network.removeIntercept", cmd.method)
		var rm struct {
			Intercept string `json:"intercept"`
		}
		if err := json.Unmarshal(cmd.params, &rm); err != nil {
			t.Fatalf("decode removeIntercept params: %v", err)
		}
		c.Check(rm.Intercept == "A", "removed intercept = %q, want A", rm.Intercept)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, errCh) == nil, "Remove A failed")

		// A confirmed removal is idempotent: no frame is sent.
		c.Check(ruleA.Remove() == nil, "second Remove A should be a local no-op")

		go func() { errCh <- ruleB.Remove() }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.removeIntercept", "command = %q, want network.removeIntercept", cmd.method)
		if err := json.Unmarshal(cmd.params, &rm); err != nil {
			t.Fatalf("decode removeIntercept params: %v", err)
		}
		c.Check(rm.Intercept == "B", "removed intercept = %q, want B", rm.Intercept)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, errCh) == nil, "Remove B failed")

		// A driver gets a single network opening.
		_, err = selenium.OpenNetwork(wd)
		c.Check(err != nil, "second OpenNetwork should fail")

		// The HTTP WebDriver session is unaffected by the network
		// connection.
		title, err := wd.Title()
		c.Check(err == nil && title == "order form", "Title = %q, %v", title, err)

		// A session without a negotiated webSocketUrl cannot open a
		// network connection.
		h.negotiateWS.Store(false)
		wd2 := h.newSession(t, c, selenium.Capabilities{"browserName": "firefox"})
		am2 := <-h.matchCh
		_, sent := am2["webSocketUrl"]
		c.Check(!sent, "alwaysMatch should not contain webSocketUrl: %v", am2)
		_, err = selenium.OpenNetwork(wd2)
		c.Check(err != nil, "OpenNetwork without negotiation should fail")

		// Closing is idempotent and leaves the HTTP session usable.
		c.Check(n.Close() == nil, "Close failed")
		c.Check(n.Close() == nil, "second Close failed")
		_, err = n.Next(context.Background())
		c.Check(err != nil, "Next after Close should fail")
		title, err = wd.Title()
		c.Check(err == nil && title == "order form", "Title after Close = %q, %v", title, err)

		c.Check(wd.Quit() == nil, "Quit failed")
		c.Check(wd2.Quit() == nil, "Quit wd2 failed")
	})
}

func TestNetworkPausedPhases(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newHarness(t)
		defer h.server.Close()
		h.negotiateWS.Store(true)
		wd := h.newSession(t, c, selenium.Capabilities{"browserName": "firefox", "webSocketUrl": true})
		<-h.matchCh
		n := h.openNetwork(t, c, wd)
		decideCh := make(chan error, 4)

		// An event that arrives before the addIntercept reply is still
		// queued for delivery.
		type aresult struct {
			i   *selenium.NetworkIntercept
			err error
		}
		addCh := make(chan aresult, 1)
		go func() {
			i, err := n.Add([]string{"beforeRequestSent", "responseStarted"}, []string{"https://form.invalid/"})
			addCh <- aresult{i, err}
		}()
		cmd := h.peer.next(t)
		c.Check(cmd.method == "network.addIntercept", "command = %q, want network.addIntercept", cmd.method)
		h.peer.send(blockedEvent("network.beforeRequestSent", "r1", 0, []string{"A"}, `,"extra":"keep"`))
		h.peer.reply(cmd.id, `{"intercept":"A"}`)
		ares := <-addCh
		c.Check(ares.err == nil && ares.i.ID() == "A", "Add A: %v", ares.err)

		// A second rule, so later events can match several rules at once.
		h.addIntercept(t, c, n, []string{"beforeRequestSent"}, nil, `{"intercept":"B"}`)

		// A cancelled Next cancels only that read.
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		_, err := n.Next(ctx)
		c.Check(err != nil, "cancelled Next should fail")

		// The early event is delivered first, with its params preserved.
		p1, err := n.Next(context.Background())
		c.Check(err == nil, "Next r1: %v", err)
		c.Check(p1.RequestID == "r1" && p1.Phase == "beforeRequestSent" && p1.RedirectCount == 0,
			"pause = %+v", p1)
		c.Check(p1.Context == "tab", "pause context = %q", p1.Context)
		c.Check(len(p1.Intercepts) == 1 && p1.Intercepts[0] == "A", "pause intercepts = %v", p1.Intercepts)
		c.Check(strings.Contains(string(p1.Raw), `"extra":"keep"`),
			"raw params lost the unknown field: %s", p1.Raw)
		go func() { decideCh <- p1.Continue(nil) }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.continueRequest", "command = %q, want network.continueRequest", cmd.method)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "Continue r1 failed")

		// An event matching both rules yields a single pause; a duplicate
		// event for the same request, redirect and phase is folded in.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r2", 0, []string{"A", "B"}, ""))
		h.peer.send(blockedEvent("network.beforeRequestSent", "r2", 0, []string{"B"}, ""))
		p2, err := n.Next(context.Background())
		c.Check(err == nil && p2.RequestID == "r2", "Next r2: %v %+v", err, p2)
		c.Check(len(p2.Intercepts) == 2, "pause intercepts = %v, want [A B]", p2.Intercepts)

		// Continue with ordered, repeated and binary headers.
		go func() {
			decideCh <- p2.Continue([]selenium.NetworkHeader{
				{Name: "X-Order", Value: selenium.NetworkBytes{Type: "string", Value: "订单-42"}},
				{Name: "X-Order", Value: selenium.NetworkBytes{Type: "string", Value: "订单-43"}},
				{Name: "X-Bin", Value: selenium.NetworkBytes{Type: "base64", Value: "AAE="}},
			})
		}()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.continueRequest", "command = %q, want network.continueRequest", cmd.method)
		var cont struct {
			Request string                   `json:"request"`
			Headers []selenium.NetworkHeader `json:"headers"`
		}
		if err := json.Unmarshal(cmd.params, &cont); err != nil {
			t.Fatalf("decode continueRequest params: %v", err)
		}
		c.Check(cont.Request == "r2", "continue request = %q, want r2", cont.Request)
		c.Check(fmt.Sprint(cont.Headers) == "[{X-Order {string 订单-42}} {X-Order {string 订单-43}} {X-Bin {base64 AAE=}}]",
			"headers = %v", cont.Headers)
		var rawCont map[string]json.RawMessage
		if err := json.Unmarshal(cmd.params, &rawCont); err != nil {
			t.Fatalf("decode continueRequest params: %v", err)
		}
		_, hasRC := rawCont["redirectCount"]
		_, hasPhase := rawCont["phase"]
		c.Check(!hasRC && !hasPhase, "command params must not carry local state: %s", cmd.params)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "Continue r2 failed")

		// The duplicate r2 event produced no extra pause: the next
		// delivered pause belongs to r3.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r3", 0, []string{"A"}, ""))
		p3, err := n.Next(context.Background())
		c.Check(err == nil && p3.RequestID == "r3", "dedup: next pause = %+v", p3)

		// nil headers omit the member entirely.
		go func() { decideCh <- p3.Continue(nil) }()
		cmd = h.peer.next(t)
		var raw3 map[string]json.RawMessage
		if err := json.Unmarshal(cmd.params, &raw3); err != nil {
			t.Fatalf("decode continueRequest params: %v", err)
		}
		_, hasHeaders := raw3["headers"]
		c.Check(!hasHeaders, "nil headers should omit the member: %s", cmd.params)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "Continue r3 failed")

		// A nonnil empty header slice clears the headers.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r4", 0, []string{"A"}, ""))
		p4, err := n.Next(context.Background())
		c.Check(err == nil && p4.RequestID == "r4", "Next r4: %v", err)
		go func() { decideCh <- p4.Continue([]selenium.NetworkHeader{}) }()
		cmd = h.peer.next(t)
		c.Check(strings.Contains(cmd.raw, `"headers":[]`), "empty headers should send []: %s", cmd.raw)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "Continue r4 failed")

		// The responseStarted phase of r5 arrives while the reply for its
		// beforeRequestSent decision is still held by the peer.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r5", 0, []string{"A"}, ""))
		p5a, err := n.Next(context.Background())
		c.Check(err == nil && p5a.RequestID == "r5", "Next r5: %v", err)
		go func() { decideCh <- p5a.Continue(nil) }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.continueRequest", "command = %q, want network.continueRequest", cmd.method)
		heldID := cmd.id

		h.peer.send(responseEvent("r5", 0, []string{"A"}))
		p5b, err := n.Next(context.Background())
		c.Check(err == nil && p5b.RequestID == "r5" && p5b.Phase == "responseStarted",
			"Next r5 response: %v %+v", err, p5b)
		go func() { decideCh <- p5b.Continue(nil) }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.continueResponse", "command = %q, want network.continueResponse", cmd.method)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "Continue r5 response failed")

		// The late reply completes only its own pause.
		h.peer.reply(heldID, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "held Continue r5 failed")

		// Both pauses are settled now; further decisions send nothing.
		c.Check(p5a.Fail() != nil, "settled pause must reject Fail")
		c.Check(p5b.Continue(nil) != nil, "settled pause must reject Continue")

		// A redirect retires the previous pause of the same request.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r6", 0, []string{"A"}, ""))
		p6a, err := n.Next(context.Background())
		c.Check(err == nil && p6a.RedirectCount == 0, "Next r6: %v %+v", err, p6a)
		h.peer.send(blockedEvent("network.beforeRequestSent", "r6", 1, []string{"A"}, ""))
		p6b, err := n.Next(context.Background())
		c.Check(err == nil && p6b.RedirectCount == 1, "Next r6 redirect: %v %+v", err, p6b)
		c.Check(p6a.Fail() != nil, "superseded pause must reject decisions")

		// A protocol error keeps the pause decidable for an explicit
		// retry, preserving the protocol code and message.
		go func() { decideCh <- p6b.Fail() }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.failRequest", "command = %q, want network.failRequest", cmd.method)
		h.peer.replyError(cmd.id, "no such request", "request r6 is gone")
		err = awaitErr(t, decideCh)
		c.Check(err != nil && strings.Contains(err.Error(), "no such request") &&
			strings.Contains(err.Error(), "request r6 is gone"),
			"protocol error = %v", err)
		go func() { decideCh <- p6b.Continue(nil) }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.continueRequest", "retry command = %q", cmd.method)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "retry after protocol error failed")
		c.Check(p6b.Fail() != nil, "completed pause must reject decisions")

		// Different requests do not block each other.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r7", 0, []string{"A"}, ""))
		h.peer.send(blockedEvent("network.beforeRequestSent", "r8", 0, []string{"A"}, ""))
		p7, err := n.Next(context.Background())
		c.Check(err == nil && p7.RequestID == "r7", "Next r7: %v %+v", err, p7)
		p8, err := n.Next(context.Background())
		c.Check(err == nil && p8.RequestID == "r8", "Next r8: %v %+v", err, p8)
		go func() { decideCh <- p8.Fail() }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.failRequest" && strings.Contains(cmd.raw, `"request":"r8"`),
			"command = %s", cmd.raw)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "Fail r8 failed")
		go func() { decideCh <- p7.Continue(nil) }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.continueRequest" && strings.Contains(cmd.raw, `"request":"r7"`),
			"command = %s", cmd.raw)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, decideCh) == nil, "Continue r7 failed")

		// Concurrent decisions on one pause produce exactly one frame.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r9", 0, []string{"A"}, ""))
		p9, err := n.Next(context.Background())
		c.Check(err == nil && p9.RequestID == "r9", "Next r9: %v", err)
		var wg sync.WaitGroup
		errs := make([]error, 2)
		wg.Add(2)
		go func() { defer wg.Done(); errs[0] = p9.Continue(nil) }()
		go func() { defer wg.Done(); errs[1] = p9.Fail() }()
		cmd = h.peer.next(t)
		var p9req struct {
			Request string `json:"request"`
		}
		if err := json.Unmarshal(cmd.params, &p9req); err != nil {
			t.Fatalf("decode decision params: %v", err)
		}
		c.Check(p9req.Request == "r9", "decision request = %q, want r9", p9req.Request)
		h.peer.reply(cmd.id, `{}`)
		wg.Wait()
		nilCount := 0
		for _, e := range errs {
			if e == nil {
				nilCount++
			}
		}
		c.Check(nilCount == 1, "concurrent decisions = %v, want exactly one success", errs)

		// Losing the connection fails the in-flight decision, all waiters
		// and later operations; the HTTP session survives.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r10", 0, []string{"A"}, ""))
		p10, err := n.Next(context.Background())
		c.Check(err == nil && p10.RequestID == "r10", "Next r10: %v", err)
		go func() { decideCh <- p10.Continue(nil) }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.continueRequest", "command = %q", cmd.method)
		h.peer.conn.Close()
		err = awaitErr(t, decideCh)
		c.Check(err != nil, "decision after connection loss should fail")
		_, err = n.Next(context.Background())
		c.Check(err != nil, "Next after connection loss should fail")
		_, err = n.Add([]string{"beforeRequestSent"}, nil)
		c.Check(err != nil, "Add after connection loss should fail")
		c.Check(p10.Fail() != nil, "pause on a dead connection must reject decisions")
		c.Check(n.Close() == nil, "Close after connection loss failed")

		title, err := wd.Title()
		c.Check(err == nil && title == "order form", "Title = %q, %v", title, err)
		c.Check(wd.Quit() == nil, "Quit failed")
	})
}

func TestNetworkRuleRemoval(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		h := newHarness(t)
		defer h.server.Close()
		h.negotiateWS.Store(true)
		wd := h.newSession(t, c, selenium.Capabilities{"browserName": "firefox", "webSocketUrl": true})
		<-h.matchCh
		n := h.openNetwork(t, c, wd)
		errCh := make(chan error, 4)

		rule, _ := h.addIntercept(t, c, n, []string{"beforeRequestSent"}, nil, `{"intercept":"A"}`)
		c.Check(rule.ID() == "A", "intercept ID = %q, want A", rule.ID())

		// A pause event arrives and stays queued while the rule is being
		// removed.
		h.peer.send(blockedEvent("network.beforeRequestSent", "r1", 0, []string{"A"}, ""))

		// A failed removal keeps the rule and reports the protocol error.
		go func() { errCh <- rule.Remove() }()
		cmd := h.peer.next(t)
		c.Check(cmd.method == "network.removeIntercept", "command = %q, want network.removeIntercept", cmd.method)
		h.peer.replyError(cmd.id, "no such intercept", "unknown intercept A")
		err := awaitErr(t, errCh)
		c.Check(err != nil && strings.Contains(err.Error(), "no such intercept") &&
			strings.Contains(err.Error(), "unknown intercept A"),
			"Remove error = %v", err)

		// Concurrent retries share a single outstanding command: the
		// second call joins the first, which is known to be in flight
		// because its command was received.
		go func() { errCh <- rule.Remove() }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.removeIntercept", "command = %q, want network.removeIntercept", cmd.method)
		go func() { errCh <- rule.Remove() }()
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, errCh) == nil, "first concurrent Remove failed")
		c.Check(awaitErr(t, errCh) == nil, "second concurrent Remove failed")

		// The pause queued before the removal survived it and remains
		// decidable.
		p, err := n.Next(context.Background())
		c.Check(err == nil && p.RequestID == "r1", "Next r1: %v %+v", err, p)
		go func() { errCh <- p.Fail() }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.failRequest", "command = %q, want network.failRequest", cmd.method)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, errCh) == nil, "Fail after removal failed")

		// The confirmed removal is idempotent: the next frame on the wire
		// belongs to the new registration below.
		c.Check(rule.Remove() == nil, "idempotent Remove failed")
		rule2, cmd2 := h.addIntercept(t, c, n, []string{"responseStarted"}, nil, `{"intercept":"B"}`)
		c.Check(rule2.ID() == "B", "intercept ID = %q, want B", rule2.ID())
		var addParams struct {
			Phases []string `json:"phases"`
		}
		if err := json.Unmarshal(cmd2.params, &addParams); err != nil {
			t.Fatalf("decode addIntercept params: %v", err)
		}
		c.Check(strings.Join(addParams.Phases, ",") == "responseStarted",
			"addIntercept phases = %v", addParams.Phases)

		// Removing the second rule and closing leave the HTTP session
		// intact.
		go func() { errCh <- rule2.Remove() }()
		cmd = h.peer.next(t)
		c.Check(cmd.method == "network.removeIntercept", "command = %q, want network.removeIntercept", cmd.method)
		h.peer.reply(cmd.id, `{}`)
		c.Check(awaitErr(t, errCh) == nil, "Remove B failed")
		c.Check(n.Close() == nil, "Close failed")

		title, err := wd.Title()
		c.Check(err == nil && title == "order form", "Title = %q, %v", title, err)
		c.Check(wd.Quit() == nil, "Quit failed")
	})
}
