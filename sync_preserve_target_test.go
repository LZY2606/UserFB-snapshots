package cdp_test

import (
	"context"
	"encoding/json"
	"sync"
	"testing"
	"time"

	"github.com/google/go-cmp/cmp"
	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/protocol/page"
	"github.com/mafredri/cdp/rpcc"
	"github.com/mafredri/cdp/task_support/taskcheck"
)

// targetEvent is the payload used by the rpcc-level tests. The timestamp
// deliberately does not follow arrival order, proving that ordering is taken
// from the actual receive path and not from protocol parameters.
type targetEvent struct {
	Value     string  `json:"v"`
	Timestamp float64 `json:"timestamp"`
}

func newStreamOrFail(c *taskcheck.C, ctx context.Context, conn *rpcc.Conn, method string) rpcc.Stream {
	s, err := rpcc.NewStream(ctx, method, conn)
	c.Check(err == nil, "NewStream(%s): %v", method, err)
	return s
}

func readyIndexes(ss []rpcc.Stream) []int {
	var got []int
	for i, s := range ss {
		select {
		case <-s.Ready():
			got = append(got, i)
		default:
		}
	}
	return got
}

func checkReady(c *taskcheck.C, ss []rpcc.Stream, want []int, label string) {
	c.Check(true, "%s", label) // Count the assertion even when equal.
	got := readyIndexes(ss)
	c.Check(cmp.Equal(got, want), "%s: ready streams = %v, want %v", label, got, want)
}

func recvEvent(c *taskcheck.C, s rpcc.Stream, label string) targetEvent {
	ch := make(chan struct {
		ev  targetEvent
		err error
	}, 1)
	go func() {
		var ev targetEvent
		err := s.RecvMsg(&ev)
		ch <- struct {
			ev  targetEvent
			err error
		}{ev, err}
	}()
	select {
	case r := <-ch:
		c.Check(r.err == nil, "%s: RecvMsg: %v", label, r.err)
		return r.ev
	case <-time.After(5 * time.Second):
		c.Check(false, "%s: timed out waiting for event", label)
		return targetEvent{}
	}
}

func eventValues(evs []targetEvent) []string {
	var got []string
	for _, ev := range evs {
		got = append(got, ev.Value)
	}
	return got
}

// fakeEvent is a third-party Stream implementation. SyncPreserve does not
// need to support these and must reject them.
type fakeEvent struct{}

func (s *fakeEvent) Ready() <-chan struct{}      { return nil }
func (s *fakeEvent) RecvMsg(m interface{}) error { return nil }
func (s *fakeEvent) Close() error                { return nil }

func TestPreserveBufferedOrder(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx := context.Background()
		conn, peer, cancel := newTestConn(t, nil)
		defer cancel()

		a := newStreamOrFail(c, ctx, conn, "test.a")
		b := newStreamOrFail(c, ctx, conn, "test.b")
		f := newStreamOrFail(c, ctx, conn, "test.f")
		cc := newStreamOrFail(c, ctx, conn, "test.c")
		d := newStreamOrFail(c, ctx, conn, "test.d")
		e := newStreamOrFail(c, ctx, conn, "test.e")

		// Two pre-existing groups plus a later independent stream.
		c.Check(rpcc.SyncPreserve(a, b, f) == nil, "SyncPreserve(a,b,f) failed")
		c.Check(rpcc.SyncPreserve(cc, d) == nil, "SyncPreserve(c,d) failed")

		// Timestamps run backwards while arrival order runs forwards.
		peer.notify("test.a", targetEvent{Value: "a0", Timestamp: 900})
		peer.notify("test.c", targetEvent{Value: "c0", Timestamp: 800})
		peer.notify("test.b", targetEvent{Value: "b0", Timestamp: 700})
		peer.notify("test.d", targetEvent{Value: "d0", Timestamp: 600})
		peer.notify("test.f", targetEvent{Value: "f0", Timestamp: 500})
		barrier(t, ctx, conn)

		// Consume a0 before the merge: consumed messages must not revive.
		first := recvEvent(c, a, "a0 pre-merge")
		c.Check(first.Value == "a0", "pre-merge event = %q, want a0", first.Value)

		peer.notify("test.a", targetEvent{Value: "a1", Timestamp: 100})
		peer.notify("test.c", targetEvent{Value: "c1", Timestamp: 200})
		peer.notify("test.f", targetEvent{Value: "f1", Timestamp: 300})
		peer.notify("test.a", targetEvent{Value: "a2", Timestamp: 400})
		peer.notify("test.e", targetEvent{Value: "e0", Timestamp: 10})
		barrier(t, ctx, conn)

		// After a0 was consumed, b holds b0, c holds c0 and e holds e0.
		// Backlogs hold the rest.
		checkReady(c, []rpcc.Stream{a, b, f, cc, d, e}, []int{1, 3, 5}, "before merge")

		// Failures must leave subscriptions, buffers, readiness and group
		// membership untouched.
		single := newStreamOrFail(c, ctx, conn, "test.single")
		defer single.Close()
		sameMethod := newStreamOrFail(c, ctx, conn, "test.a")
		defer sameMethod.Close()
		closed := newStreamOrFail(c, ctx, conn, "test.closed")
		c.Check(closed.Close() == nil, "Close(closed)")
		conn2, _, cancel2 := newTestConn(t, nil)
		defer cancel2()
		other := newStreamOrFail(c, ctx, conn2, "test.other")
		defer other.Close()

		var nilEvent rpcc.Stream
		failures := []struct {
			name string
			err  error
		}{
			{"single stream", rpcc.SyncPreserve(a)},
			{"nil stream", rpcc.SyncPreserve(a, nilEvent)},
			{"duplicate object", rpcc.SyncPreserve(a, a)},
			{"third-party stream", rpcc.SyncPreserve(a, &fakeEvent{})},
			{"duplicate method", rpcc.SyncPreserve(a, b, f, sameMethod)},
			{"cross connection", rpcc.SyncPreserve(a, b, f, other)},
			{"closed stream", rpcc.SyncPreserve(a, b, f, closed)},
			{"partial old group", rpcc.SyncPreserve(a, b)},
			{"partial old group with new stream", rpcc.SyncPreserve(a, b, e)},
		}
		for _, failure := range failures {
			c.Check(failure.err != nil, "%s: expected an error, got nil", failure.name)
			checkReady(c, []rpcc.Stream{a, b, f, cc, d, e}, []int{1, 3, 5}, "after failed "+failure.name)
		}

		// Merge the two backlogged groups and the independent stream.
		err := rpcc.SyncPreserve(a, b, f, cc, d, e)
		c.Check(err == nil, "SyncPreserve merge: %v", err)

		// Only the very first pending event may signal Ready.
		checkReady(c, []rpcc.Stream{a, b, f, cc, d, e}, []int{3}, "after merge")

		order := []rpcc.Stream{cc, b, d, f, a, cc, f, a, e}
		want := []string{"c0", "b0", "d0", "f0", "a1", "c1", "f1", "a2", "e0"}
		for i, s := range order {
			// Exactly one stream is ready for each next event: messages
			// queued behind another stream must not light Ready early.
			var ready []int
			for j, ss := range []rpcc.Stream{a, b, f, cc, d, e} {
				if selectReady(ss) {
					ready = append(ready, j)
				}
			}
			wantReady := []int{indexOf([]rpcc.Stream{a, b, f, cc, d, e}, s)}
			c.Check(cmp.Equal(ready, wantReady), "step %d: ready = %v, want %v", i, ready, wantReady)
			ev := recvEvent(c, s, want[i])
			c.Check(ev.Value == want[i], "step %d: got %q, want %q", i, ev.Value, want[i])
		}
		c.Check(len(order) == len(want), "test table mismatch")

		// New events after the merge are delivered too, and closing a
		// member discards its unconsumed events while the rest proceed.
		peer.notify("test.a", targetEvent{Value: "a3", Timestamp: 1})
		peer.notify("test.d", targetEvent{Value: "d1", Timestamp: 1})
		peer.notify("test.c", targetEvent{Value: "c2", Timestamp: 1})
		barrier(t, ctx, conn)
		c.Check(d.Close() == nil, "Close(d)")
		got := []targetEvent{recvEvent(c, a, "a3"), recvEvent(c, cc, "c2")}
		c.Check(cmp.Equal(eventValues(got), []string{"a3", "c2"}), "after close member: got %v", eventValues(got))

		// A group created by the legacy Sync can also be merged. Its
		// buffered events are empty by construction (Sync clears them),
		// but events that arrive after Sync are preserved.
		g1 := newStreamOrFail(c, ctx, conn, "test.g1")
		g2 := newStreamOrFail(c, ctx, conn, "test.g2")
		defer g1.Close()
		defer g2.Close()
		c.Check(rpcc.Sync(g1, g2) == nil, "legacy Sync")
		peer.notify("test.g1", targetEvent{Value: "g1.0", Timestamp: 1})
		peer.notify("test.g2", targetEvent{Value: "g2.0", Timestamp: 1})
		peer.notify("test.a", targetEvent{Value: "a4", Timestamp: 1})
		barrier(t, ctx, conn)
		err = rpcc.SyncPreserve(a, b, f, cc, e, g1, g2)
		c.Check(err == nil, "merge legacy group: %v", err)
		for _, step := range []struct {
			s    rpcc.Stream
			want string
		}{
			{g1, "g1.0"}, {g2, "g2.0"}, {a, "a4"},
		} {
			ev := recvEvent(c, step.s, step.want)
			c.Check(ev.Value == step.want, "legacy merge: got %q, want %q", ev.Value, step.want)
		}
	})
}

func selectReady(s rpcc.Stream) bool {
	select {
	case <-s.Ready():
		return true
	default:
		return false
	}
}

func indexOf(ss []rpcc.Stream, s rpcc.Stream) int {
	for i := range ss {
		if ss[i] == s {
			return i
		}
	}
	return -1
}

func TestPreserveHandoff(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx := context.Background()
		conn, peer, cancel := newTestConn(t, nil)
		defer cancel()

		a := newStreamOrFail(c, ctx, conn, "h.a")
		b := newStreamOrFail(c, ctx, conn, "h.b")
		cc := newStreamOrFail(c, ctx, conn, "h.c")

		c.Check(rpcc.SyncPreserve(a, b) == nil, "SyncPreserve(a,b)")

		// Events buffered before the handoff.
		peer.notify("h.a", targetEvent{Value: "h0", Timestamp: 90})
		peer.notify("h.b", targetEvent{Value: "h1", Timestamp: 80})
		barrier(t, ctx, conn)
		checkReady(c, []rpcc.Stream{a, b, cc}, []int{0}, "before handoff")

		// Notifications continue while the merge is running: the handoff
		// boundary must neither lose nor duplicate messages.
		var wg sync.WaitGroup
		wg.Add(1)
		started := make(chan struct{})
		go func() {
			defer wg.Done()
			peer.notify("h.a", targetEvent{Value: "h2", Timestamp: 70})
			close(started)
			peer.notify("h.b", targetEvent{Value: "h3", Timestamp: 60})
			peer.notify("h.a", targetEvent{Value: "h4", Timestamp: 50})
			peer.notify("h.c", targetEvent{Value: "h5", Timestamp: 40})
			peer.notify("h.b", targetEvent{Value: "h6", Timestamp: 30})
		}()
		<-started
		err := rpcc.SyncPreserve(a, b, cc)
		wg.Wait()
		c.Check(err == nil, "SyncPreserve during handoff: %v", err)
		barrier(t, ctx, conn)

		// First pending event is h0 (for a); b and c must stay closed.
		checkReady(c, []rpcc.Stream{a, b, cc}, []int{0}, "after handoff")
		var got []string
		got = append(got, recvEvent(c, a, "h0").Value)
		for _, step := range []struct {
			s    rpcc.Stream
			want string
		}{
			{b, "h1"}, {a, "h2"}, {b, "h3"}, {a, "h4"}, {cc, "h5"}, {b, "h6"},
		} {
			ev := recvEvent(c, step.s, step.want)
			got = append(got, ev.Value)
		}
		c.Check(cmp.Equal(got, []string{"h0", "h1", "h2", "h3", "h4", "h5", "h6"}), "handoff order: got %v", got)

		// Repeated reorganization: two existing groups are merged again
		// with an independent stream, preserving their buffered events.
		d := newStreamOrFail(c, ctx, conn, "h.d")
		e := newStreamOrFail(c, ctx, conn, "h.e")
		c.Check(rpcc.SyncPreserve(d, e) == nil, "SyncPreserve(d,e)")
		peer.notify("h.c", targetEvent{Value: "r0", Timestamp: 5})
		peer.notify("h.d", targetEvent{Value: "r1", Timestamp: 4})
		peer.notify("h.e", targetEvent{Value: "r2", Timestamp: 3})
		peer.notify("h.a", targetEvent{Value: "r3", Timestamp: 2})
		barrier(t, ctx, conn)
		// Both independent groups now have a pending message; each group's
		// first-in-line member may be ready, but not a member queued behind
		// another event within its own group.
		checkReady(c, []rpcc.Stream{a, b, cc, d, e}, []int{2, 3}, "before second merge")
		c.Check(rpcc.SyncPreserve(a, b, cc, d, e) == nil, "second merge")
		got = nil
		for _, step := range []struct {
			s    rpcc.Stream
			want string
		}{
			{cc, "r0"}, {d, "r1"}, {e, "r2"}, {a, "r3"},
		} {
			ev := recvEvent(c, step.s, step.want)
			got = append(got, ev.Value)
		}
		c.Check(cmp.Equal(got, []string{"r0", "r1", "r2", "r3"}), "second merge order: got %v", got)

		// Closing a group member discards its unconsumed events, and the
		// remaining members continue with later events.
		peer.notify("h.a", targetEvent{Value: "r4", Timestamp: 1})
		peer.notify("h.e", targetEvent{Value: "r5", Timestamp: 1})
		peer.notify("h.b", targetEvent{Value: "r6", Timestamp: 1})
		barrier(t, ctx, conn)
		c.Check(e.Close() == nil, "Close(e)")
		got = nil
		got = append(got, recvEvent(c, a, "r4").Value, recvEvent(c, b, "r6").Value)
		c.Check(cmp.Equal(got, []string{"r4", "r6"}), "after closing member: got %v", got)

		// The group still works after the close.
		peer.notify("h.a", targetEvent{Value: "r7", Timestamp: 1})
		peer.notify("h.b", targetEvent{Value: "r8", Timestamp: 1})
		barrier(t, ctx, conn)
		got = nil
		got = append(got, recvEvent(c, a, "r7").Value, recvEvent(c, b, "r8").Value)
		c.Check(cmp.Equal(got, []string{"r7", "r8"}), "group after close: got %v", got)
	})
}

func TestPreserveGenerated(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx := context.Background()
		conn, peer, cancel := newTestConn(t, nil)
		defer cancel()

		client := cdp.NewClient(conn)

		nav, err := client.Page.FrameNavigated(ctx)
		c.Check(err == nil, "Page.FrameNavigated: %v", err)
		dialog, err := client.Page.JavascriptDialogOpening(ctx)
		c.Check(err == nil, "Page.JavascriptDialogOpening: %v", err)
		request, err := client.Network.RequestWillBeSent(ctx)
		c.Check(err == nil, "Network.RequestWillBeSent: %v", err)
		if nav == nil || dialog == nil || request == nil {
			return
		}
		defer nav.Close()
		defer dialog.Close()
		defer request.Close()

		// A bypass subscriber on a method that also takes part in the
		// merge. It must keep receiving the complete sequence.
		bypass, err := rpcc.NewStream(ctx, "Page.frameNavigated", conn)
		c.Check(err == nil, "bypass NewStream: %v", err)
		if bypass == nil {
			return
		}
		defer bypass.Close()

		navEvent := func(url string) map[string]interface{} {
			return map[string]interface{}{
				"frame": map[string]interface{}{
					"id":       "F1",
					"loaderId": "L1",
					"url":      url,
				},
				"type": "Navigation",
			}
		}
		dialogEvent := func(message string) map[string]interface{} {
			return map[string]interface{}{
				"url":               "https://example.test/",
				"frameId":           "F1",
				"message":           message,
				"type":              "confirm",
				"hasBrowserHandler": true,
			}
		}
		requestEvent := func(url string, timestamp float64) map[string]interface{} {
			return map[string]interface{}{
				"requestId":   "R-" + url,
				"loaderId":    "L1",
				"documentURL": "https://example.test/",
				"request": map[string]interface{}{
					"url":             url,
					"method":          "POST",
					"headers":         map[string]string{},
					"initialPriority": "High",
					"referrerPolicy":  "no-referrer",
				},
				"timestamp": timestamp,
				"wallTime":  timestamp,
				"initiator": map[string]interface{}{"type": "parser"},
			}
		}

		// Buffered before the merge, with timestamps in reverse order.
		peer.notify("Page.frameNavigated", navEvent("https://one.test/"))
		peer.notify("Page.javascriptDialogOpening", dialogEvent("confirm-1"))
		peer.notify("Network.requestWillBeSent", requestEvent("https://req-one.test/", 9))
		barrier(t, ctx, conn)

		// Real generated clients participate directly; the client is not
		// rebuilt.
		err = cdp.SyncPreserve(nav, dialog, request)
		c.Check(err == nil, "cdp.SyncPreserve: %v", err)

		// The first buffered Page event is a navigation.
		checkReady(c, []rpcc.Stream{nav, dialog, request}, []int{0}, "generated after merge")

		nav1, err := nav.Recv()
		c.Check(err == nil, "Recv nav1: %v", err)
		if nav1 != nil {
			c.Check(nav1.Frame.URL == "https://one.test/", "nav1 URL = %q", nav1.Frame.URL)
		}
		dialog1, err := dialog.Recv()
		c.Check(err == nil, "Recv dialog1: %v", err)
		if dialog1 != nil {
			c.Check(dialog1.Message == "confirm-1", "dialog1 = %q", dialog1.Message)
		}
		req1, err := request.Recv()
		c.Check(err == nil, "Recv req1: %v", err)
		if req1 != nil {
			c.Check(req1.Request.URL == "https://req-one.test/", "req1 URL = %q", req1.Request.URL)
			c.Check(float64(req1.Timestamp) == 9, "req1 timestamp = %v", float64(req1.Timestamp))
		}

		// A group member that arrives after the merge is delivered after
		// everything that was buffered.
		peer.notify("Page.frameNavigated", navEvent("https://two.test/"))
		peer.notify("Page.javascriptDialogOpening", dialogEvent("confirm-2"))
		peer.notify("Network.requestWillBeSent", requestEvent("https://req-two.test/", 1))
		barrier(t, ctx, conn)

		nav2, err := nav.Recv()
		c.Check(err == nil, "Recv nav2: %v", err)
		if nav2 != nil {
			c.Check(nav2.Frame.URL == "https://two.test/", "nav2 URL = %q", nav2.Frame.URL)
		}
		dialog2, err := dialog.Recv()
		c.Check(err == nil, "Recv dialog2: %v", err)
		if dialog2 != nil {
			c.Check(dialog2.Message == "confirm-2", "dialog2 = %q", dialog2.Message)
		}
		req2, err := request.Recv()
		c.Check(err == nil, "Recv req2: %v", err)
		if req2 != nil {
			c.Check(req2.Request.URL == "https://req-two.test/", "req2 URL = %q", req2.Request.URL)
			c.Check(float64(req2.Timestamp) == 1, "req2 timestamp = %v", float64(req2.Timestamp))
		}

		// An independent subscriber to the same method as a group member
		// keeps receiving all events, including ones after the merge.
		navPeer, err := client.Page.FrameNavigated(ctx)
		c.Check(err == nil, "second FrameNavigated client: %v", err)
		if navPeer != nil {
			defer navPeer.Close()
			peer.notify("Page.frameNavigated", navEvent("https://three.test/"))
			barrier(t, ctx, conn)
			nav3, err := nav.Recv()
			c.Check(err == nil, "Recv nav3: %v", err)
			if nav3 != nil {
				c.Check(nav3.Frame.URL == "https://three.test/", "nav3 URL = %q", nav3.Frame.URL)
			}
			navPeer3, err := navPeer.Recv()
			c.Check(err == nil, "Recv navPeer3: %v", err)
			if navPeer3 != nil {
				c.Check(navPeer3.Frame.URL == "https://three.test/", "navPeer3 URL = %q", navPeer3.Frame.URL)
			}
		}

		// The bypass subscriber saw the complete navigation sequence.
		var navs []string
		for i := 0; i < 3; i++ {
			var raw []byte
			read := make(chan error, 1)
			go func() { read <- bypass.RecvMsg(&raw) }()
			select {
			case err := <-read:
				c.Check(err == nil, "bypass RecvMsg: %v", err)
			case <-time.After(5 * time.Second):
				c.Check(false, "bypass: timed out")
			}
			var reply page.FrameNavigatedReply
			if err := json.Unmarshal(raw, &reply); err != nil {
				c.Check(false, "bypass unmarshal: %v", err)
				continue
			}
			navs = append(navs, reply.Frame.URL)
		}
		c.Check(cmp.Equal(navs, []string{
			"https://one.test/",
			"https://two.test/",
			"https://three.test/",
		}), "bypass sequence = %v", navs)

		// Ordinary command replies are unaffected by the group.
		c.Check(client.Network.Enable(ctx, nil) == nil, "Network.Enable after merge")
	})
}
