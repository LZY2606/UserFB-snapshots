package godet

import (
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"

	"github.com/raff/godet/task_support/taskcheck"
)

// cdpOutcome is the captured result of a NavigateAndWait goroutine.
type cdpOutcome struct {
	result NavigationResult
	err    error
}

// cdpHandler is the DevTools-shaped handler used by the fake endpoint:
//   - GET /json/list returns a page tab with a websocket URL
//   - the websocket records every command and auto-acknowledges everything
//     except Page.navigate, whose reply timing the test controls
func cdpHandler(h *cdpHarness) http.Handler {
	upgrader := websocket.Upgrader{}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/json/list" {
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode([]map[string]string{{
				"id": "page-1", "type": "page",
				"webSocketDebuggerUrl": h.endpoint,
			}})
			return
		}

		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			return
		}
		h.conns <- conn

		done := make(chan struct{})
		go func() { // single writer for this connection
			for {
				select {
				case msg := <-h.send:
					if conn.WriteJSON(msg) != nil {
						return
					}
				case <-done:
					return
				}
			}
		}()

		for {
			var cmd Params
			if err := conn.ReadJSON(&cmd); err != nil {
				close(done)
				return
			}

			select {
			case h.commands <- cmd:
			default:
				h.t.Errorf("harness command channel full: %v", cmd)
			}

			// Page.navigate replies are scripted by the test itself.
			if cmd.String("method") == "Page.navigate" {
				continue
			}

			var result any = Params{}
			if cmd.String("method") == "Runtime.evaluate" {
				result = Params{"result": Params{"type": "string", "value": "ready"}}
			}
			// All writes go through the single writer goroutine.
			h.send <- Params{"id": cmd["id"], "result": result}
		}
	})
}

// cdpHarness is a local HTTP/WebSocket endpoint that speaks enough of the
// DevTools protocol to drive the real godet code without Chrome.
type cdpHarness struct {
	t        *testing.T
	server   *httptest.Server
	endpoint string
	commands chan Params
	send     chan Params
	conns    chan *websocket.Conn
}

func newCDPHarness(t *testing.T) *cdpHarness {
	h := &cdpHarness{
		t:        t,
		commands: make(chan Params, 64),
		send:     make(chan Params, 64),
		conns:    make(chan *websocket.Conn, 4),
	}
	h.server = httptest.NewServer(cdpHandler(h))
	h.endpoint = "ws" + strings.TrimPrefix(h.server.URL, "http") + "/devtools/page/page-1"
	t.Cleanup(h.server.Close)
	return h
}

func (h *cdpHarness) connect(c *taskcheck.C) *RemoteDebugger {
	remote, err := Connect(strings.TrimPrefix(h.server.URL, "http://"), false)
	c.Check(err == nil, "Connect failed: %v", err)
	if remote != nil {
		h.t.Cleanup(func() { _ = remote.Close() })
	}
	return remote
}

func (h *cdpHarness) waitCommand(c *taskcheck.C) Params {
	select {
	case cmd := <-h.commands:
		return cmd
	case <-time.After(5 * time.Second):
		c.Check(false, "timed out waiting for a command")
		return nil
	}
}

func (h *cdpHarness) checkNoCommand(c *taskcheck.C) {
	select {
	case cmd := <-h.commands:
		c.Check(false, "unexpected command was sent: %v", cmd)
	case <-time.After(100 * time.Millisecond):
		c.Check(true, "no command sent")
	}
}

func (h *cdpHarness) push(msg Params) { h.send <- msg }

func (h *cdpHarness) extra() *cdpHarness { return newCDPHarness(h.t) }

func (h *cdpHarness) dropConnection(c *taskcheck.C) {
	select {
	case conn := <-h.conns:
		c.Check(conn != nil, "server accepted the websocket")
		_ = conn.Close()
	case <-time.After(5 * time.Second):
		c.Check(false, "timed out waiting for the server connection")
	}
}

func navParam(cmd Params, key string) string {
	return Params(cmd.Map("params")).String(key)
}

func navReply(id int, result Params) Params {
	return Params{"id": id, "result": result}
}

func lifecycleEvent(frameID, loaderID, name string) Params {
	return Params{
		"method": "Page.lifecycleEvent",
		"params": Params{"frameId": frameID, "loaderId": loaderID, "name": name},
	}
}

func loadEventFired() Params {
	return Params{"method": "Page.loadEventFired", "params": Params{"timestamp": 1.0}}
}

func startNavigateAndWait(remote *RemoteDebugger, ctx context.Context, url string, trans TransitionType, event string) chan cdpOutcome {
	ch := make(chan cdpOutcome, 1)
	go func() {
		result, err := remote.NavigateAndWait(ctx, url, trans, event)
		ch <- cdpOutcome{result: result, err: err}
	}()
	return ch
}

func waitOutcome(c *taskcheck.C, ch chan cdpOutcome) cdpOutcome {
	select {
	case out := <-ch:
		return out
	case <-time.After(5 * time.Second):
		c.Check(false, "timed out waiting for NavigateAndWait to return")
		return cdpOutcome{err: errors.New("timeout")}
	}
}

func assertNotDone(c *taskcheck.C, ch chan cdpOutcome, label string) {
	select {
	case out := <-ch:
		c.Check(false, "%s resolved early: %+v / %v", label, out.result, out.err)
	case <-time.After(100 * time.Millisecond):
		c.Check(true, "%s still waiting", label)
	}
}

// TestNavigationLoader covers the lifecycle matching rules: events arriving
// before and after the command reply, same-document jumps, unrelated events
// that must not substitute, duplicate events and navigation failure.
func TestNavigationLoader(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		navigationLoaderScenarios(t, c, newCDPHarness(t))
	})
}

func navigationLoaderScenarios(t *testing.T, c *taskcheck.C, h *cdpHarness) {
	remote := h.connect(c)
	ctx := context.Background()

	// Lifecycle events arrive before the Page.navigate reply and must not
	// be lost, nor may they complete the call before the reply.
	early := startNavigateAndWait(remote, ctx, "https://forms.invalid/early", NoTransition, LifecycleLoad)
	cmd := h.waitCommand(c)
	c.Check(cmd.String("method") == "Page.navigate", "command method: %v", cmd)
	c.Check(navParam(cmd, "url") == "https://forms.invalid/early", "command url: %v", cmd)

	h.push(lifecycleEvent("frame-1", "loader-1", LifecycleDOMContentLoaded))
	h.push(lifecycleEvent("frame-1", "loader-1", LifecycleLoad))
	assertNotDone(c, early, "early-event navigation")

	h.push(navReply(cmd.Int("id"), Params{"frameId": "frame-1", "loaderId": "loader-1"}))
	out := waitOutcome(c, early)
	c.Check(out.err == nil, "early-event navigation error: %v", out.err)
	c.Check(out.result.FrameID == "frame-1", "frame id: %q", out.result.FrameID)
	c.Check(out.result.LoaderID == "loader-1", "loader id: %q", out.result.LoaderID)
	c.Check(!out.result.SameDocument, "new document must not be SameDocument")

	// Reply first, then a mix of unrelated events that must not complete
	// the call: sibling frame, old loader, Page.loadEventFired, an
	// unobserved lifecycle name and a duplicate matching event.
	late := startNavigateAndWait(remote, ctx, "https://forms.invalid/late", NoTransition, LifecycleDOMContentLoaded)
	cmd = h.waitCommand(c)
	h.push(navReply(cmd.Int("id"), Params{"frameId": "frame-2", "loaderId": "loader-2"}))
	assertNotDone(c, late, "post-reply navigation without lifecycle event")

	h.push(lifecycleEvent("other-frame", "loader-2", LifecycleDOMContentLoaded)) // sibling frame
	h.push(lifecycleEvent("frame-2", "old-loader", LifecycleDOMContentLoaded))   // old loader
	h.push(lifecycleEvent("frame-2", "loader-2", "init"))                        // other name
	h.push(loadEventFired())                                                     // different event type
	assertNotDone(c, late, "navigation after unrelated events")

	h.push(lifecycleEvent("frame-2", "loader-2", LifecycleDOMContentLoaded))
	h.push(lifecycleEvent("frame-2", "loader-2", LifecycleDOMContentLoaded)) // duplicate
	out = waitOutcome(c, late)
	c.Check(out.err == nil, "late-event navigation error: %v", out.err)
	c.Check(out.result.FrameID == "frame-2" && out.result.LoaderID == "loader-2",
		"late-event result: %+v", out.result)
	c.Check(!out.result.SameDocument, "new document must not be SameDocument")

	// frameId without loaderId: same-document navigation returns at once.
	sameDoc := startNavigateAndWait(remote, ctx, "https://forms.invalid/anchor", NoTransition, LifecycleLoad)
	cmd = h.waitCommand(c)
	h.push(navReply(cmd.Int("id"), Params{"frameId": "frame-3"}))
	out = waitOutcome(c, sameDoc)
	c.Check(out.err == nil, "same-document navigation error: %v", out.err)
	c.Check(out.result.SameDocument, "expected SameDocument, got %+v", out.result)
	c.Check(out.result.FrameID == "frame-3" && out.result.LoaderID == "",
		"same-document result: %+v", out.result)

	// errorText produces a NavigationError, never a success result.
	failed := startNavigateAndWait(remote, ctx, "https://forms.invalid/broken", NoTransition, LifecycleLoad)
	cmd = h.waitCommand(c)
	h.push(navReply(cmd.Int("id"), Params{
		"frameId": "frame-4", "loaderId": "loader-4", "errorText": "net::ERR_FAILED",
	}))
	out = waitOutcome(c, failed)
	c.Check(out.err != nil, "navigation failure should return an error")
	var navErr NavigationError
	c.Check(errors.As(out.err, &navErr), "expected NavigationError, got %T %v", out.err, out.err)
	c.Check(string(navErr) == "net::ERR_FAILED", "navigation error text: %q", navErr)
	c.Check(out.result == (NavigationResult{}), "failed navigation must not return a result")
}

// TestNavigationRequest covers command sending and the context/connection
// lifecycle: validation before sending, pre-canceled contexts, cancellation
// while waiting for the reply followed by a fresh navigation, command shape,
// Close and websocket disconnect.
func TestNavigationRequest(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		navigationRequestScenarios(t, c, newCDPHarness(t))
	})
}

func navigationRequestScenarios(t *testing.T, c *taskcheck.C, h *cdpHarness) {
	remote := h.connect(c)

	// Invalid lifecycle event is rejected before anything is written.
	_, err := remote.NavigateAndWait(context.Background(), "https://forms.invalid/bad", NoTransition, "complete")
	c.Check(errors.Is(err, ErrInvalidLifecycleEvent), "invalid event error: %v", err)
	h.checkNoCommand(c)

	// An already-canceled context sends no command at all.
	canceledCtx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err = remote.NavigateAndWait(canceledCtx, "https://forms.invalid/precancel", NoTransition, LifecycleLoad)
	c.Check(errors.Is(err, ErrNavigationCanceled), "pre-canceled error: %v", err)
	c.Check(errors.Is(err, context.Canceled), "error should wrap context.Canceled: %v", err)
	h.checkNoCommand(c)

	// Cancel while waiting for the command reply, then navigate again: the
	// canceled call sent exactly one command, its late reply and events are
	// discarded, and the next call completes on its own loader.
	ctx1, cancel1 := context.WithCancel(context.Background())
	first := startNavigateAndWait(remote, ctx1, "https://forms.invalid/canceled", Reload, LifecycleLoad)
	cmd1 := h.waitCommand(c)
	c.Check(cmd1.String("method") == "Page.navigate", "first command: %v", cmd1)
	c.Check(navParam(cmd1, "transitionType") == "reload", "transition type preserved: %v", cmd1)

	cancel1()
	out := waitOutcome(c, first)
	c.Check(errors.Is(out.err, ErrNavigationCanceled), "canceled navigation error: %v", out.err)

	// Late traffic for the canceled navigation must not affect anything.
	h.push(navReply(cmd1.Int("id"), Params{"frameId": "frame-old", "loaderId": "loader-old"}))
	h.push(lifecycleEvent("frame-old", "loader-old", LifecycleLoad))
	h.push(lifecycleEvent("frame-old", "loader-old", LifecycleLoad)) // duplicate

	second := startNavigateAndWait(remote, context.Background(), "https://forms.invalid/again", NoTransition, LifecycleDOMContentLoaded)
	cmd2 := h.waitCommand(c)
	c.Check(cmd2.Int("id") != cmd1.Int("id"), "new navigation must use a fresh request id: %d vs %d",
		cmd2.Int("id"), cmd1.Int("id"))
	c.Check(navParam(cmd2, "url") == "https://forms.invalid/again", "second command url: %v", cmd2)
	_, hasTransition := cmd2.Map("params")["transitionType"]
	c.Check(!hasTransition, "NoTransition must omit transitionType: %v", cmd2)

	h.push(lifecycleEvent("frame-new", "loader-new", LifecycleDOMContentLoaded))
	h.push(navReply(cmd2.Int("id"), Params{"frameId": "frame-new", "loaderId": "loader-new"}))
	out = waitOutcome(c, second)
	c.Check(out.err == nil, "second navigation error: %v", out.err)
	c.Check(out.result.LoaderID == "loader-new", "second navigation used its own loader: %+v", out.result)

	// RemoteDebugger.Close ends an in-progress wait with a recognizable error.
	closeHarness := h.extra()
	closeRemote := closeHarness.connect(c)
	closing := startNavigateAndWait(closeRemote, context.Background(), "https://forms.invalid/closing", NoTransition, LifecycleLoad)
	_ = closeHarness.waitCommand(c)
	_ = closeRemote.Close()
	out = waitOutcome(c, closing)
	c.Check(errors.Is(out.err, ErrNavigationClosed), "Close should fail the wait: %v", out.err)
	_, err = closeRemote.NavigateAndWait(context.Background(), "https://forms.invalid/after", NoTransition, LifecycleLoad)
	c.Check(errors.Is(err, ErrorClose), "call after Close: %v", err)

	// A websocket disconnect also ends the wait.
	discHarness := h.extra()
	discRemote := discHarness.connect(c)
	dropping := startNavigateAndWait(discRemote, context.Background(), "https://forms.invalid/drop", NoTransition, LifecycleLoad)
	_ = discHarness.waitCommand(c)
	discHarness.dropConnection(c)
	out = waitOutcome(c, dropping)
	c.Check(errors.Is(out.err, ErrNavigationClosed), "disconnect should fail the wait: %v", out.err)
}

// TestNavigationObservers covers concurrent navigations and the independence
// of the internal observer machinery from user CallbackEvent callbacks,
// including blocked and replaced callbacks.
func TestNavigationObservers(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		navigationObserversScenarios(t, c, newCDPHarness(t))
	})
}

func navigationObserversScenarios(t *testing.T, c *taskcheck.C, h *cdpHarness) {
	remote := h.connect(c)

	// Interleaved waits: each call is bound to its own reply and loader;
	// events and replies arrive in an interleaved order.
	navA := startNavigateAndWait(remote, context.Background(), "https://forms.invalid/a", NoTransition, LifecycleLoad)
	navB := startNavigateAndWait(remote, context.Background(), "https://forms.invalid/b", NoTransition, LifecycleDOMContentLoaded)

	cmdA := h.waitCommand(c)
	cmdB := h.waitCommand(c)
	commandsByURL := map[string]Params{
		navParam(cmdA, "url"): cmdA,
		navParam(cmdB, "url"): cmdB,
	}
	c.Check(commandsByURL["https://forms.invalid/a"] != nil, "missing command for url a: %v %v", cmdA, cmdB)
	c.Check(commandsByURL["https://forms.invalid/b"] != nil, "missing command for url b: %v %v", cmdA, cmdB)
	c.Check(cmdA.Int("id") != cmdB.Int("id"), "interleaved commands need distinct ids: %v %v", cmdA, cmdB)
	idA := commandsByURL["https://forms.invalid/a"].Int("id")
	idB := commandsByURL["https://forms.invalid/b"].Int("id")

	// A's DOMContentLoaded is not the event A waits for; B's wanted event
	// arrives before B's reply and must be buffered, not lost.
	h.push(lifecycleEvent("frame-a", "loader-a", LifecycleDOMContentLoaded))
	h.push(lifecycleEvent("frame-b", "loader-b", LifecycleDOMContentLoaded))
	h.push(navReply(idB, Params{"frameId": "frame-b", "loaderId": "loader-b"}))
	outB := waitOutcome(c, navB)
	c.Check(outB.err == nil, "nav B error: %v", outB.err)
	c.Check(outB.result == (NavigationResult{FrameID: "frame-b", LoaderID: "loader-b"}),
		"nav B result: %+v", outB.result)
	assertNotDone(c, navA, "nav A still waiting for load")

	h.push(navReply(idA, Params{"frameId": "frame-a", "loaderId": "loader-a"}))
	assertNotDone(c, navA, "nav A after reply without load event")
	h.push(lifecycleEvent("frame-a", "loader-a", LifecycleLoad))
	outA := waitOutcome(c, navA)
	c.Check(outA.err == nil, "nav A error: %v", outA.err)
	c.Check(outA.result == (NavigationResult{FrameID: "frame-a", LoaderID: "loader-a"}),
		"nav A result: %+v", outA.result)

	// Canceling one of two interleaved calls must not tear down the other.
	// The two goroutines can reach the send channel in either order, so map
	// the observed commands by URL instead of assuming wire order.
	ctxX, cancelX := context.WithCancel(context.Background())
	navX := startNavigateAndWait(remote, ctxX, "https://forms.invalid/x", NoTransition, LifecycleLoad)
	navY := startNavigateAndWait(remote, context.Background(), "https://forms.invalid/y", NoTransition, LifecycleDOMContentLoaded)
	seenX := h.waitCommand(c)
	seenY := h.waitCommand(c)
	xyByURL := map[string]Params{
		navParam(seenX, "url"): seenX,
		navParam(seenY, "url"): seenY,
	}
	cmdX := xyByURL["https://forms.invalid/x"]
	cmdY := xyByURL["https://forms.invalid/y"]
	c.Check(cmdX != nil && cmdY != nil, "missing x/y commands: %v %v", seenX, seenY)
	c.Check(cmdX.Int("id") != cmdY.Int("id"), "x/y commands need distinct ids")

	cancelX()
	outX := waitOutcome(c, navX)
	c.Check(errors.Is(outX.err, ErrNavigationCanceled), "canceled nav X: %v", outX.err)

	// Late traffic belonging to X cannot complete Y.
	h.push(navReply(cmdX.Int("id"), Params{"frameId": "frame-x", "loaderId": "loader-x"}))
	h.push(lifecycleEvent("frame-x", "loader-x", LifecycleLoad))
	h.push(lifecycleEvent("frame-y", "loader-x", LifecycleDOMContentLoaded)) // mismatched ids
	assertNotDone(c, navY, "nav Y unaffected by nav X traffic")
	h.push(lifecycleEvent("frame-y", "loader-y", LifecycleDOMContentLoaded))
	h.push(navReply(cmdY.Int("id"), Params{"frameId": "frame-y", "loaderId": "loader-y"}))
	outY := waitOutcome(c, navY)
	c.Check(outY.err == nil, "nav Y error: %v", outY.err)
	c.Check(outY.result.LoaderID == "loader-y", "nav Y result: %+v", outY.result)

	// A blocked user callback must not stall navigation confirmation; the
	// internal observer never occupies the CallbackEvent slot.
	blockHarness := h.extra()
	blockRemote := blockHarness.connect(c)

	entered := make(chan struct{}, 4)
	block := make(chan struct{})
	blockRemote.CallbackEvent("Page.lifecycleEvent", func(Params) {
		entered <- struct{}{}
		<-block
	})

	blockedNav := startNavigateAndWait(blockRemote, context.Background(), "https://forms.invalid/blocked", NoTransition, LifecycleLoad)
	blockCmd := blockHarness.waitCommand(c)
	blockHarness.push(lifecycleEvent("frame-block", "loader-block", LifecycleLoad))
	blockHarness.push(navReply(blockCmd.Int("id"), Params{"frameId": "frame-block", "loaderId": "loader-block"}))

	select {
	case <-entered:
		c.Check(true, "user callback was invoked")
	case <-time.After(5 * time.Second):
		c.Check(false, "user callback never invoked")
	}
	out := waitOutcome(c, blockedNav)
	c.Check(out.err == nil, "navigation completed while user callback blocked: %v", out.err)
	c.Check(out.result.LoaderID == "loader-block", "blocked-callback result: %+v", out.result)
	close(block)

	// Replacing the callback mid-flight keeps the user callback chain
	// working and preserves event delivery order.
	var mu sync.Mutex
	var seen []string
	swapped := make(chan struct{}, 8)
	blockRemote.CallbackEvent("Page.lifecycleEvent", func(params Params) {
		mu.Lock()
		seen = append(seen, params.String("name"))
		mu.Unlock()
		swapped <- struct{}{}
	})

	swapNav := startNavigateAndWait(blockRemote, context.Background(), "https://forms.invalid/swap", NoTransition, LifecycleLoad)
	swapCmd := blockHarness.waitCommand(c)
	blockHarness.push(lifecycleEvent("frame-swap", "loader-swap", LifecycleDOMContentLoaded))
	blockHarness.push(lifecycleEvent("frame-swap", "loader-swap", LifecycleLoad))
	blockHarness.push(navReply(swapCmd.Int("id"), Params{"frameId": "frame-swap", "loaderId": "loader-swap"}))
	out = waitOutcome(c, swapNav)
	c.Check(out.err == nil, "swap navigation error: %v", out.err)

	for i := 0; i < 2; i++ {
		select {
		case <-swapped:
		case <-time.After(5 * time.Second):
			c.Check(false, "replaced callback did not receive event %d", i)
		}
	}
	mu.Lock()
	c.Check(len(seen) == 2 && seen[0] == LifecycleDOMContentLoaded && seen[1] == LifecycleLoad,
		"callback event order: %v", seen)
	mu.Unlock()
}
