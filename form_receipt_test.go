package godet_test

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
	"github.com/raff/godet"
	"github.com/raff/godet/task_support/taskcheck"
)

// formTestServer is a local HTTP/WebSocket endpoint speaking just enough
// of the DevTools protocol to drive Connect/Evaluate and to inject
// Network events on the real command/event path.
type formTestServer struct {
	server   *httptest.Server
	commands chan map[string]interface{}

	mu        sync.Mutex
	ws        *websocket.Conn
	evalErr   bool          // reply to Runtime.evaluate with a JS error
	evalBlock chan struct{} // if non-nil, hold the Runtime.evaluate reply until closed
}

func newFormTestServer() *formTestServer {
	f := &formTestServer{commands: make(chan map[string]interface{}, 64)}

	mux := http.NewServeMux()
	mux.HandleFunc("/json/list", func(w http.ResponseWriter, r *http.Request) {
		json.NewEncoder(w).Encode([]map[string]interface{}{{
			"id":                   "one",
			"type":                 "page",
			"webSocketDebuggerUrl": "ws" + strings.TrimPrefix(f.server.URL, "http") + "/page/one",
		}})
	})
	mux.HandleFunc("/page/one", func(w http.ResponseWriter, r *http.Request) {
		ws, err := (&websocket.Upgrader{}).Upgrade(w, r, nil)
		if err != nil {
			return
		}

		f.mu.Lock()
		f.ws = ws
		f.mu.Unlock()

		for {
			var q map[string]interface{}
			if err := ws.ReadJSON(&q); err != nil {
				return
			}

			f.commands <- q

			reply := map[string]interface{}{"id": q["id"]}

			if q["method"] == "Runtime.evaluate" {
				f.mu.Lock()
				block := f.evalBlock
				evalErr := f.evalErr
				f.mu.Unlock()

				if block != nil {
					<-block // hold the reply until the test releases it
				}

				if evalErr {
					reply["result"] = map[string]interface{}{
						"result": map[string]interface{}{
							"type":        "object",
							"subtype":     "error",
							"class":       "Error",
							"description": "EvalError: submit failed",
						},
						"exceptionDetails": map[string]interface{}{"text": "submit failed"},
					}
				} else {
					reply["result"] = map[string]interface{}{
						"result": map[string]interface{}{"type": "undefined"},
					}
				}
			} else {
				reply["result"] = map[string]interface{}{}
			}

			f.mu.Lock()
			err = ws.WriteJSON(reply)
			f.mu.Unlock()

			if err != nil {
				return
			}
		}
	})

	f.server = httptest.NewServer(mux)
	return f
}

// sendEvent injects one protocol event into the websocket.
func (f *formTestServer) sendEvent(t *testing.T, method string, params map[string]interface{}) {
	t.Helper()

	f.mu.Lock()
	defer f.mu.Unlock()

	if f.ws == nil {
		t.Fatal("no websocket connection")
	}
	if err := f.ws.WriteJSON(map[string]interface{}{"method": method, "params": params}); err != nil {
		t.Fatalf("send event %s: %v", method, err)
	}
}

// nextCommand is a channel barrier: it returns the next command the
// client sent over the websocket.
func (f *formTestServer) nextCommand(t *testing.T) map[string]interface{} {
	t.Helper()

	select {
	case q := <-f.commands:
		return q
	case <-time.After(10 * time.Second):
		t.Fatal("timed out waiting for client command")
		return nil
	}
}

func (f *formTestServer) setEvalErr(v bool) {
	f.mu.Lock()
	f.evalErr = v
	f.mu.Unlock()
}

func (f *formTestServer) setEvalBlock(ch chan struct{}) {
	f.mu.Lock()
	f.evalBlock = ch
	f.mu.Unlock()
}

// closeAbnormal closes the websocket with an abnormal close code.
func (f *formTestServer) closeAbnormal() {
	f.mu.Lock()
	defer f.mu.Unlock()

	if f.ws != nil {
		f.ws.WriteMessage(websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.CloseInternalServerErr, "boom"))
		f.ws.Close()
	}
}

// connectForm connects to the fake browser and enables Network events,
// as callers of WaitFormReceipt are expected to do.
func connectForm(t *testing.T, f *formTestServer) *godet.RemoteDebugger {
	t.Helper()

	remote, err := godet.Connect(strings.TrimPrefix(f.server.URL, "http://"), false)
	if err != nil {
		t.Fatal(err)
	}

	if err := remote.NetworkEvents(true); err != nil {
		t.Fatal(err)
	}

	if q := f.nextCommand(t); q["method"] != "Network.enable" {
		t.Fatalf("expected Network.enable, got %v", q["method"])
	}

	return remote
}

// expectEvaluate is a barrier: the next client command must be the
// Runtime.evaluate issued by the wait action.
func expectEvaluate(t *testing.T, f *formTestServer) {
	t.Helper()

	if q := f.nextCommand(t); q["method"] != "Runtime.evaluate" {
		t.Fatalf("expected Runtime.evaluate, got %v", q["method"])
	}
}

// submitAction triggers the form submission through the real command path.
func submitAction(remote *godet.RemoteDebugger, count *int32) func() error {
	return func() error {
		atomic.AddInt32(count, 1)
		_, err := remote.Evaluate("document.querySelector('form').requestSubmit()")
		return err
	}
}

type waitOutcome struct {
	receipt *godet.FormReceipt
	err     error
}

// waitAsync starts WaitFormReceipt in the background.
func waitAsync(remote *godet.RemoteDebugger, ctx context.Context, match godet.FormRequestMatch, action func() error) chan waitOutcome {
	ch := make(chan waitOutcome, 1)
	go func() {
		receipt, err := remote.WaitFormReceipt(ctx, match, action)
		ch <- waitOutcome{receipt, err}
	}()
	return ch
}

// awaitOutcome is a channel barrier for a pending wait.
func awaitOutcome(t *testing.T, ch chan waitOutcome) waitOutcome {
	t.Helper()

	select {
	case res := <-ch:
		return res
	case <-time.After(10 * time.Second):
		t.Fatal("timed out waiting for WaitFormReceipt")
		return waitOutcome{}
	}
}

// requestEvent builds a Network.requestWillBeSent event.
func requestEvent(reqID, frameID, url, method string, redirect map[string]interface{}, redirectExtra bool) map[string]interface{} {
	ev := map[string]interface{}{
		"requestId": reqID,
		"frameId":   frameID,
		"request":   map[string]interface{}{"url": url, "method": method},
	}
	if redirect != nil {
		ev["redirectResponse"] = redirect
		ev["redirectHasExtraInfo"] = redirectExtra
	}
	return ev
}

// responseEvent builds a Network.responseReceived event.
func responseEvent(reqID, url string, status int, headers map[string]interface{}, hasExtra bool) map[string]interface{} {
	return map[string]interface{}{
		"requestId":    reqID,
		"frameId":      "frame-1",
		"response":     map[string]interface{}{"url": url, "status": status, "headers": headers},
		"hasExtraInfo": hasExtra,
	}
}

// extraEvent builds a Network.responseReceivedExtraInfo event.
func extraEvent(reqID string, status int, headers map[string]interface{}) map[string]interface{} {
	return map[string]interface{}{
		"requestId":  reqID,
		"statusCode": status,
		"headers":    headers,
	}
}

func TestFormReceiptHops(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := newFormTestServer()
		defer f.server.Close()

		remote := connectForm(t, f)
		defer remote.Close()

		// barrier: fires when the observer has processed the second
		// (late) extra info event
		var extraCount int32
		extraSeen := make(chan struct{})
		remote.CallbackEvent("Network.responseReceivedExtraInfo", func(godet.Params) {
			if atomic.AddInt32(&extraCount, 1) == 2 {
				close(extraSeen)
			}
		})

		// hold the evaluate reply so the network can complete before the
		// action returns
		evalBlock := make(chan struct{})
		f.setEvalBlock(evalBlock)

		match := godet.FormRequestMatch{FrameID: "frame-1", URL: "http://app.local/form", Method: "POST"}

		var actions int32
		resultCh := waitAsync(remote, context.Background(), match, submitAction(remote, &actions))

		expectEvaluate(t, f)

		const req1 = "req-1"

		// hop 0: POST /form
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent(req1, "frame-1", "http://app.local/form", "POST", nil, false))

		// extra info for hop 0 arrives before the response that closes it
		f.sendEvent(t, "Network.responseReceivedExtraInfo",
			extraEvent(req1, 302, map[string]interface{}{"X-Wire": "hop0"}))

		// redirect to /step1, closing hop 0 (extra info declared)
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent(req1, "frame-1", "http://app.local/step1", "GET",
				map[string]interface{}{
					"url":     "http://app.local/form",
					"status":  302,
					"headers": map[string]interface{}{"Location": "/step1"},
				}, true))

		// redirect to /final, closing hop 1 (no extra info declared)
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent(req1, "frame-1", "http://app.local/final", "GET",
				map[string]interface{}{
					"url":     "http://app.local/step1",
					"status":  302,
					"headers": map[string]interface{}{"Location": "/final"},
				}, false))

		// final response: 200 on the wire protocol response
		f.sendEvent(t, "Network.responseReceived",
			responseEvent(req1, "http://app.local/final", 200,
				map[string]interface{}{"Content-Type": "text/html"}, true))

		f.sendEvent(t, "Network.loadingFinished", map[string]interface{}{"requestId": req1})

		// late extra info for the last hop, after loadingFinished; the
		// wire status differs from the response status
		f.sendEvent(t, "Network.responseReceivedExtraInfo",
			extraEvent(req1, 304, map[string]interface{}{"X-Wire": "hop2"}))

		// wait until the observer has seen the late extra info
		select {
		case <-extraSeen:
		case <-time.After(10 * time.Second):
			t.Fatal("observer did not process the late extra info")
		}

		// the network side is complete, but the action has not returned
		// yet: no receipt may be delivered
		select {
		case res := <-resultCh:
			t.Fatalf("receipt delivered before the action returned: %+v", res)
		default:
		}

		// let the evaluate reply through: the action succeeds now
		close(evalBlock)

		res := awaitOutcome(t, resultCh)
		c.Check(res.err == nil, "WaitFormReceipt error: %v", res.err)
		c.Check(atomic.LoadInt32(&actions) == 1, "action ran %d times, want 1", atomic.LoadInt32(&actions))

		receipt := res.receipt
		c.Check(receipt != nil, "receipt must not be nil")
		if receipt == nil {
			return
		}

		c.Check(receipt.RequestID == req1, "request id = %q, want %q", receipt.RequestID, req1)
		c.Check(len(receipt.Hops) == 3, "hop count = %d, want 3", len(receipt.Hops))
		if len(receipt.Hops) != 3 {
			return
		}

		hop0 := receipt.Hops[0]
		c.Check(hop0.URL == "http://app.local/form", "hop0 URL = %q", hop0.URL)
		c.Check(hop0.Method == "POST", "hop0 method = %q", hop0.Method)
		c.Check(hop0.Status == 302, "hop0 status = %d", hop0.Status)
		c.Check(hop0.Headers["Location"] == "/step1", "hop0 Location = %q", hop0.Headers["Location"])
		c.Check(hop0.HasExtraInfo, "hop0 should declare extra info")
		c.Check(hop0.WireStatus == 302, "hop0 wire status = %d", hop0.WireStatus)
		c.Check(hop0.ExtraHeaders["X-Wire"] == "hop0", "hop0 wire headers = %v", hop0.ExtraHeaders)

		hop1 := receipt.Hops[1]
		c.Check(hop1.URL == "http://app.local/step1", "hop1 URL = %q", hop1.URL)
		c.Check(hop1.Method == "GET", "hop1 method = %q", hop1.Method)
		c.Check(hop1.Status == 302, "hop1 status = %d", hop1.Status)
		c.Check(hop1.Headers["Location"] == "/final", "hop1 Location = %q", hop1.Headers["Location"])
		c.Check(!hop1.HasExtraInfo, "hop1 should not declare extra info")
		c.Check(hop1.WireStatus == 0, "hop1 wire status = %d, want 0", hop1.WireStatus)
		c.Check(hop1.ExtraHeaders == nil, "hop1 wire headers = %v, want nil", hop1.ExtraHeaders)

		hop2 := receipt.Hops[2]
		c.Check(hop2.URL == "http://app.local/final", "hop2 URL = %q", hop2.URL)
		c.Check(hop2.Method == "GET", "hop2 method = %q", hop2.Method)
		c.Check(hop2.Status == 200, "hop2 status = %d", hop2.Status)
		c.Check(hop2.Headers["Content-Type"] == "text/html", "hop2 Content-Type = %q", hop2.Headers["Content-Type"])
		c.Check(hop2.HasExtraInfo, "hop2 should declare extra info")
		c.Check(hop2.WireStatus == 304, "hop2 wire status = %d, want 304", hop2.WireStatus)
		c.Check(hop2.ExtraHeaders["X-Wire"] == "hop2", "hop2 wire headers = %v", hop2.ExtraHeaders)
	})
}

func TestFormReceiptIsolation(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := newFormTestServer()
		defer f.server.Close()

		remote := connectForm(t, f)
		defer remote.Close()

		// a normal user callback coexists with the observers; it also
		// trashes the event params to prove the receipt is a snapshot
		var seen int32
		remote.CallbackEvent("Network.requestWillBeSent", func(p godet.Params) {
			atomic.AddInt32(&seen, 1)
			p["requestId"] = "corrupted"
			if req := p.Map("request"); req != nil {
				req["url"] = "corrupted"
			}
		})

		match := godet.FormRequestMatch{FrameID: "frame-1", URL: "http://app.local/same", Method: "POST"}

		var actionsA, actionsB int32
		ctxB, cancelB := context.WithCancel(context.Background())

		resA := waitAsync(remote, context.Background(), match, submitAction(remote, &actionsA))
		resB := waitAsync(remote, ctxB, match, submitAction(remote, &actionsB))

		// both actions go through the real command path
		expectEvaluate(t, f)
		expectEvaluate(t, f)

		// extra info for the future request id arrives before the
		// request id is known: it must be stashed by the observers
		f.sendEvent(t, "Network.responseReceivedExtraInfo",
			extraEvent("req-1", 299, map[string]interface{}{"X-Wire": "early"}))

		// noise: a sibling frame posting the same URL, an image, and an
		// extra info for an unrelated request id
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("sib-1", "frame-2", "http://app.local/same", "POST", nil, false))
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("img-1", "frame-1", "http://app.local/logo.png", "GET", nil, false))
		f.sendEvent(t, "Network.responseReceivedExtraInfo",
			extraEvent("stray", 599, map[string]interface{}{"X-Wire": "stray"}))

		// the first exact match fixes the request id for both waits
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("req-1", "frame-1", "http://app.local/same", "POST", nil, false))

		// a second submission of the same URL in the same frame must not
		// take the observation over
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("req-2", "frame-1", "http://app.local/same", "POST", nil, false))

		// cancel wait B: only its own observer is torn down
		cancelB()
		resBResult := awaitOutcome(t, resB)
		c.Check(errors.Is(resBResult.err, context.Canceled),
			"wait B error = %v, want context.Canceled", resBResult.err)
		c.Check(resBResult.receipt == nil, "wait B receipt = %v, want nil", resBResult.receipt)

		// noise on the second submission
		f.sendEvent(t, "Network.responseReceived",
			responseEvent("req-2", "http://app.local/same", 500, nil, false))
		f.sendEvent(t, "Network.loadingFinished", map[string]interface{}{"requestId": "req-2"})

		// complete the observed request; its extra info comes from the
		// stashed early event
		f.sendEvent(t, "Network.responseReceived",
			responseEvent("req-1", "http://app.local/same", 200,
				map[string]interface{}{"X-Result": "ok"}, true))
		f.sendEvent(t, "Network.loadingFinished", map[string]interface{}{"requestId": "req-1"})

		resAResult := awaitOutcome(t, resA)
		c.Check(resAResult.err == nil, "wait A error: %v", resAResult.err)

		receipt := resAResult.receipt
		c.Check(receipt != nil, "receipt must not be nil")
		if receipt == nil {
			return
		}

		c.Check(receipt.RequestID == "req-1", "request id = %q, want req-1", receipt.RequestID)
		c.Check(len(receipt.Hops) == 1, "hop count = %d, want 1", len(receipt.Hops))
		if len(receipt.Hops) != 1 {
			return
		}

		hop := receipt.Hops[0]
		c.Check(hop.URL == "http://app.local/same", "hop URL = %q", hop.URL)
		c.Check(hop.Method == "POST", "hop method = %q", hop.Method)
		c.Check(hop.Status == 200, "hop status = %d", hop.Status)
		c.Check(hop.Headers["X-Result"] == "ok", "hop X-Result = %q", hop.Headers["X-Result"])
		c.Check(hop.HasExtraInfo, "hop should declare extra info")
		c.Check(hop.WireStatus == 299, "hop wire status = %d, want 299", hop.WireStatus)
		c.Check(hop.ExtraHeaders["X-Wire"] == "early", "hop wire headers = %v", hop.ExtraHeaders)

		c.Check(atomic.LoadInt32(&actionsA) == 1, "action A ran %d times, want 1", atomic.LoadInt32(&actionsA))
		c.Check(atomic.LoadInt32(&actionsB) == 1, "action B ran %d times, want 1", atomic.LoadInt32(&actionsB))
		c.Check(atomic.LoadInt32(&seen) == 4,
			"user callback saw %d requestWillBeSent events, want 4", atomic.LoadInt32(&seen))
	})
}

func TestFormReceiptCompletion(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		f := newFormTestServer()
		defer f.server.Close()

		remote := connectForm(t, f)
		defer remote.Close()

		match := godet.FormRequestMatch{FrameID: "frame-1", URL: "http://app.local/apply", Method: "POST"}

		// --- round 1: plain success; a stray extra info must not leak
		var a1 int32
		res1 := waitAsync(remote, context.Background(), match, submitAction(remote, &a1))
		expectEvaluate(t, f)

		f.sendEvent(t, "Network.responseReceivedExtraInfo",
			extraEvent("zz", 511, map[string]interface{}{"X-Wire": "stray"}))
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("req-r1", "frame-1", "http://app.local/apply", "POST", nil, false))
		f.sendEvent(t, "Network.responseReceived",
			responseEvent("req-r1", "http://app.local/apply", 200,
				map[string]interface{}{"X-Result": "round1"}, false))
		f.sendEvent(t, "Network.loadingFinished", map[string]interface{}{"requestId": "req-r1"})

		out1 := awaitOutcome(t, res1)
		c.Check(out1.err == nil, "round 1 error: %v", out1.err)
		c.Check(atomic.LoadInt32(&a1) == 1, "round 1 action ran %d times, want 1", atomic.LoadInt32(&a1))
		ok1 := out1.receipt != nil && len(out1.receipt.Hops) == 1
		c.Check(ok1, "round 1 receipt = %+v", out1.receipt)
		if ok1 {
			hop := out1.receipt.Hops[0]
			c.Check(hop.Status == 200 && hop.Headers["X-Result"] == "round1",
				"round 1 hop = %+v", hop)
			c.Check(!hop.HasExtraInfo && hop.WireStatus == 0 && hop.ExtraHeaders == nil,
				"round 1 hop should have no extra info: %+v", hop)
		}

		// --- round 2: the next round after completion stays clean; the
		// stray extra info from round 1 must not satisfy this hop
		var a2 int32
		res2 := waitAsync(remote, context.Background(), match, submitAction(remote, &a2))
		expectEvaluate(t, f)

		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("req-r2", "frame-1", "http://app.local/apply", "POST", nil, false))
		f.sendEvent(t, "Network.responseReceived",
			responseEvent("req-r2", "http://app.local/apply", 201, nil, true))
		f.sendEvent(t, "Network.loadingFinished", map[string]interface{}{"requestId": "req-r2"})
		f.sendEvent(t, "Network.responseReceivedExtraInfo",
			extraEvent("req-r2", 201, map[string]interface{}{"X-Wire": "round2"}))

		out2 := awaitOutcome(t, res2)
		c.Check(out2.err == nil, "round 2 error: %v", out2.err)
		c.Check(atomic.LoadInt32(&a2) == 1, "round 2 action ran %d times, want 1", atomic.LoadInt32(&a2))
		ok2 := out2.receipt != nil && len(out2.receipt.Hops) == 1
		c.Check(ok2, "round 2 receipt = %+v", out2.receipt)
		if ok2 {
			hop := out2.receipt.Hops[0]
			c.Check(hop.Status == 201, "round 2 hop status = %d", hop.Status)
			c.Check(hop.WireStatus == 201, "round 2 wire status = %d, want 201 (stale extras must not leak)", hop.WireStatus)
			c.Check(hop.ExtraHeaders["X-Wire"] == "round2", "round 2 wire headers = %v", hop.ExtraHeaders)
		}

		// --- round 3: loadingFailed delivers a request error, not a receipt
		var a3 int32
		res3 := waitAsync(remote, context.Background(), match, submitAction(remote, &a3))
		expectEvaluate(t, f)

		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("req-f1", "frame-1", "http://app.local/apply", "POST", nil, false))
		f.sendEvent(t, "Network.loadingFailed", map[string]interface{}{
			"requestId": "req-f1",
			"errorText": "net::ERR_CONNECTION_RESET",
		})

		out3 := awaitOutcome(t, res3)
		c.Check(out3.receipt == nil, "round 3 receipt = %v, want nil", out3.receipt)
		var reqErr *godet.FormRequestError
		asReqErr := errors.As(out3.err, &reqErr)
		c.Check(asReqErr, "round 3 error = %v, want *FormRequestError", out3.err)
		if asReqErr {
			c.Check(reqErr.RequestID == "req-f1", "round 3 error request id = %q", reqErr.RequestID)
			c.Check(reqErr.ErrorText == "net::ERR_CONNECTION_RESET", "round 3 error text = %q", reqErr.ErrorText)
		}
		c.Check(atomic.LoadInt32(&a3) == 1, "round 3 action ran %d times, want 1", atomic.LoadInt32(&a3))

		// --- round 4: an action error is returned as-is
		f.setEvalErr(true)
		var a4 int32
		res4 := waitAsync(remote, context.Background(), match, submitAction(remote, &a4))
		expectEvaluate(t, f)

		out4 := awaitOutcome(t, res4)
		c.Check(out4.receipt == nil, "round 4 receipt = %v, want nil", out4.receipt)
		var evalErr godet.EvaluateError
		c.Check(errors.As(out4.err, &evalErr), "round 4 error = %v, want EvaluateError", out4.err)
		c.Check(out4.err != nil && strings.Contains(out4.err.Error(), "submit failed"),
			"round 4 error = %v, want the action error as-is", out4.err)
		c.Check(atomic.LoadInt32(&a4) == 1, "round 4 action ran %d times, want 1", atomic.LoadInt32(&a4))
		f.setEvalErr(false)

		// --- round 5: cancellation ends the wait; late events for the
		// cancelled observation must not re-settle anything
		cancelMatch := godet.FormRequestMatch{FrameID: "frame-1", URL: "http://app.local/cancelled", Method: "POST"}
		ctx5, cancel5 := context.WithCancel(context.Background())
		var a5 int32
		res5 := waitAsync(remote, ctx5, cancelMatch, submitAction(remote, &a5))
		expectEvaluate(t, f)

		cancel5()
		out5 := awaitOutcome(t, res5)
		c.Check(errors.Is(out5.err, context.Canceled), "round 5 error = %v, want context.Canceled", out5.err)
		c.Check(out5.receipt == nil, "round 5 receipt = %v, want nil", out5.receipt)
		c.Check(atomic.LoadInt32(&a5) == 1, "round 5 action ran %d times, want 1", atomic.LoadInt32(&a5))

		// late events for the cancelled observation
		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("req-c1", "frame-1", "http://app.local/cancelled", "POST", nil, false))
		f.sendEvent(t, "Network.responseReceived",
			responseEvent("req-c1", "http://app.local/cancelled", 200, nil, false))
		f.sendEvent(t, "Network.loadingFinished", map[string]interface{}{"requestId": "req-c1"})

		// --- round 6: the connection is still fully usable, and an HTTP
		// error status is a deliverable response, not a load failure
		var a6 int32
		res6 := waitAsync(remote, context.Background(), match, submitAction(remote, &a6))
		expectEvaluate(t, f)

		f.sendEvent(t, "Network.requestWillBeSent",
			requestEvent("req-r6", "frame-1", "http://app.local/apply", "POST", nil, false))
		f.sendEvent(t, "Network.responseReceived",
			responseEvent("req-r6", "http://app.local/apply", 404, nil, false))
		f.sendEvent(t, "Network.loadingFinished", map[string]interface{}{"requestId": "req-r6"})

		out6 := awaitOutcome(t, res6)
		c.Check(out6.err == nil, "round 6 error: %v", out6.err)
		c.Check(atomic.LoadInt32(&a6) == 1, "round 6 action ran %d times, want 1", atomic.LoadInt32(&a6))
		c.Check(out6.receipt != nil, "round 6 receipt must not be nil")
		if out6.receipt != nil {
			c.Check(out6.receipt.RequestID == "req-r6",
				"round 6 request id = %q, want req-r6", out6.receipt.RequestID)
			if len(out6.receipt.Hops) == 1 {
				c.Check(out6.receipt.Hops[0].Status == 404,
					"round 6 hop status = %d, want 404", out6.receipt.Hops[0].Status)
			} else {
				c.Check(false, "round 6 hop count = %d, want 1", len(out6.receipt.Hops))
			}
		}

		// --- round 7: Close during a wait ends it
		var a7 int32
		res7 := waitAsync(remote, context.Background(), match, submitAction(remote, &a7))
		expectEvaluate(t, f)

		if err := remote.Close(); err != nil {
			t.Fatalf("close: %v", err)
		}

		out7 := awaitOutcome(t, res7)
		c.Check(errors.Is(out7.err, godet.ErrorClose), "round 7 error = %v, want ErrorClose", out7.err)
		c.Check(out7.receipt == nil, "round 7 receipt = %v, want nil", out7.receipt)

		// --- round 8: a server-side disconnect during a wait ends it
		remote2 := connectForm(t, f)
		defer remote2.Close()

		var a8 int32
		res8 := waitAsync(remote2, context.Background(), match, submitAction(remote2, &a8))
		expectEvaluate(t, f)

		f.closeAbnormal()

		out8 := awaitOutcome(t, res8)
		c.Check(errors.Is(out8.err, godet.ErrorFormDisconnected),
			"round 8 error = %v, want ErrorFormDisconnected", out8.err)
		c.Check(out8.receipt == nil, "round 8 receipt = %v, want nil", out8.receipt)
	})
}
