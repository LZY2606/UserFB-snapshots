package godet

import (
	"encoding/base64"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/raff/godet/task_support/taskcheck"
)

// helpers driving the screenPeer fixture through the production Connect path

type startResult struct {
	s   *Screencast
	err error
}

func b64(s string) string {
	return base64.StdEncoding.EncodeToString([]byte(s))
}

func frameMeta() Params {
	return Params{
		"pageScaleFactor": 1.0,
		"viewport":        Params{"x": 0.0, "y": 0.0},
	}
}

func expectCmd(c *taskcheck.C, p *screenPeer, method string) Params {
	select {
	case m := <-p.commands:
		c.Check(m["method"] == method, "expected command %s, got %v", method, m)
		if m["method"] != method {
			return nil
		}
		return m
	case <-time.After(5 * time.Second):
		c.Check(false, "missing command %s", method)
		return nil
	}
}

func expectAck(c *taskcheck.C, p *screenPeer, sessionID int) Params {
	m := expectCmd(c, p, "Page.screencastFrameAck")
	if m == nil {
		return nil
	}
	prm, _ := m["params"].(map[string]interface{})
	c.Check(prm["sessionId"] == float64(sessionID), "ack sessionId %d, got %v", sessionID, prm)
	return m
}

func expectNoCmd(c *taskcheck.C, p *screenPeer, wait time.Duration) {
	select {
	case m := <-p.commands:
		c.Check(false, "unexpected command %v", m)
	case <-time.After(wait):
	}
}

func replyErr(t *testing.T, p *screenPeer, m Params, code int, message string) {
	t.Helper()
	p.write(t, Params{"id": m["id"], "error": Params{"code": code, "message": message}})
}

func awaitStart(c *taskcheck.C, ch chan startResult) startResult {
	select {
	case r := <-ch:
		return r
	case <-time.After(5 * time.Second):
		c.Check(false, "StartScreencast did not return")
		return startResult{}
	}
}

func awaitErr(c *taskcheck.C, ch chan error) error {
	select {
	case err := <-ch:
		return err
	case <-time.After(5 * time.Second):
		c.Check(false, "operation did not return")
		return fmt.Errorf("timeout")
	}
}

func recvFrame(c *taskcheck.C, s *Screencast) (ScreencastFrame, bool) {
	select {
	case f, ok := <-s.Frames():
		if !ok {
			c.Check(false, "frames channel closed unexpectedly")
			return ScreencastFrame{}, false
		}
		return f, true
	case <-time.After(5 * time.Second):
		c.Check(false, "timeout waiting for frame")
		return ScreencastFrame{}, false
	}
}

func expectNoFrame(c *taskcheck.C, s *Screencast, wait time.Duration) {
	select {
	case f, ok := <-s.Frames():
		c.Check(false, "unexpected frame (ok=%v): %+v", ok, f)
	case <-time.After(wait):
	}
}

func waitSignal(c *taskcheck.C, ch chan struct{}, what string) {
	select {
	case <-ch:
	case <-time.After(5 * time.Second):
		c.Check(false, "timeout waiting for %s", what)
	}
}

// awaitClosed drains any frames still in the mailbox until the channel is closed.
func awaitClosed(c *taskcheck.C, s *Screencast) bool {
	deadline := time.After(5 * time.Second)
	for {
		select {
		case _, ok := <-s.Frames():
			if !ok {
				return true
			}
		case <-deadline:
			c.Check(false, "frames channel not closed")
			return false
		}
	}
}

func TestScreencastStart(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p := newScreenPeer(t)
		d, err := Connect(strings.TrimPrefix(p.server.URL, "http://"), false)
		c.Check(err == nil, "connect: %v", err)
		if err != nil {
			return
		}
		defer d.Close()

		// user callback mutates the event params (nested metadata and data);
		// delivered frames must not be affected
		mutated := make(chan struct{}, 16)
		d.CallbackEvent("Page.screencastFrame", func(v Params) {
			if md := v.Map("metadata"); md != nil {
				md["pageScaleFactor"] = 42.0
				if vp, ok := md["viewport"].(map[string]interface{}); ok {
					vp["x"] = 999.0
					vp["injected"] = true
				}
			}
			v["data"] = b64("corrupted")
			mutated <- struct{}{}
		})

		startCh := make(chan startResult, 1)
		go func() {
			s, err := d.StartScreencast()
			startCh <- startResult{s, err}
		}()

		m := expectCmd(c, p, "Page.startScreencast")
		if m == nil {
			return
		}
		prm, _ := m["params"].(map[string]interface{})
		c.Check(prm["format"] == "png", "startScreencast format: %v", prm)
		c.Check(prm["everyNthFrame"] == 1.0, "startScreencast everyNthFrame: %v", prm)

		// early frame F1 arrives before the start reply: acked immediately
		p.event(t, "Page.screencastFrame", Params{"sessionId": 1, "data": b64("frame-1"), "metadata": frameMeta()})
		a1 := expectAck(c, p, 1)
		if a1 == nil {
			return
		}
		p.reply(t, a1, Params{})
		waitSignal(c, mutated, "user callback for frame 1")

		// early frame F2: ack is delayed past the start reply
		p.event(t, "Page.screencastFrame", Params{"sessionId": 2, "data": b64("frame-2"), "metadata": frameMeta()})
		a2 := expectAck(c, p, 2)
		if a2 == nil {
			return
		}

		// start succeeds
		p.reply(t, m, Params{})
		res := awaitStart(c, startCh)
		c.Check(res.err == nil && res.s != nil, "start: %v", res.err)
		if res.err != nil || res.s == nil {
			return
		}
		s := res.s

		// the latest acknowledged early frame (F1) is delivered, pristine
		// despite the user callback mutating its own copy of the params
		f, ok := recvFrame(c, s)
		c.Check(ok && f.SessionID == 1, "first delivered frame: %+v", f)
		c.Check(string(f.Data) == "frame-1", "frame data: %q", f.Data)
		c.Check(f.Metadata.Int("pageScaleFactor") == 1, "metadata mutated: %v", f.Metadata)
		vp := f.Metadata.Map("viewport")
		c.Check(vp != nil && vp["x"] == 0.0 && vp["injected"] == nil, "nested metadata mutated: %v", vp)
		// F2 is not acknowledged yet, so it is not delivered
		expectNoFrame(c, s, 150*time.Millisecond)

		// the delayed ack of F2 makes it deliverable
		p.reply(t, a2, Params{})
		f, ok = recvFrame(c, s)
		c.Check(ok && f.SessionID == 2 && string(f.Data) == "frame-2", "frame after delayed ack: %+v", f)

		// a blocked, unrelated user callback must not stall acks or delivery
		release := make(chan struct{})
		entered := make(chan struct{}, 1)
		d.CallbackEvent("Network.requestWillBeSent", func(v Params) {
			entered <- struct{}{}
			<-release
		})
		p.event(t, "Network.requestWillBeSent", Params{"requestId": "r1"})
		waitSignal(c, entered, "blocking callback")

		p.event(t, "Page.screencastFrame", Params{"sessionId": 3, "data": b64("frame-3"), "metadata": frameMeta()})
		a3 := expectAck(c, p, 3)
		if a3 == nil {
			close(release)
			return
		}
		p.reply(t, a3, Params{})
		f, ok = recvFrame(c, s)
		c.Check(ok && f.SessionID == 3 && string(f.Data) == "frame-3", "frame with blocked callback: %+v", f)
		close(release)

		// a second start while running is rejected without sending commands
		s2, err := d.StartScreencast()
		c.Check(err == ErrorScreencastActive, "second start error: %v", err)
		c.Check(s2 == nil, "second start stream: %v", s2)
		expectNoCmd(c, p, 150*time.Millisecond)

		// stop the stream cleanly
		stopCh := make(chan error, 1)
		go func() { stopCh <- s.Stop() }()
		sm := expectCmd(c, p, "Page.stopScreencast")
		if sm == nil {
			return
		}
		p.reply(t, sm, Params{})
		c.Check(awaitErr(c, stopCh) == nil, "stop")

		// start failing with a CDP error: code/message preserved, no frames
		// published, ownership released
		go func() {
			s, err := d.StartScreencast()
			startCh <- startResult{s, err}
		}()
		m = expectCmd(c, p, "Page.startScreencast")
		if m == nil {
			return
		}
		// an early frame is still acknowledged even though the start fails
		p.event(t, "Page.screencastFrame", Params{"sessionId": 4, "data": b64("frame-4"), "metadata": frameMeta()})
		a4 := expectAck(c, p, 4)
		if a4 == nil {
			return
		}
		p.reply(t, a4, Params{})
		replyErr(t, p, m, -32601, "no screencast for you")
		res = awaitStart(c, startCh)
		c.Check(res.s == nil, "failed start returned a stream: %v", res.s)
		rerr, isRespErr := res.err.(*ResponseError)
		c.Check(isRespErr && rerr.Code == -32601 && rerr.Message == "no screencast for you",
			"start error: %#v", res.err)

		// ownership was released: a new start succeeds
		go func() {
			s, err := d.StartScreencast()
			startCh <- startResult{s, err}
		}()
		m = expectCmd(c, p, "Page.startScreencast")
		if m == nil {
			return
		}
		p.reply(t, m, Params{})
		res = awaitStart(c, startCh)
		c.Check(res.err == nil && res.s != nil, "restart after failure: %v", res.err)
		if res.err != nil || res.s == nil {
			return
		}
		s = res.s

		go func() { stopCh <- s.Stop() }()
		sm = expectCmd(c, p, "Page.stopScreencast")
		if sm == nil {
			return
		}
		p.reply(t, sm, Params{})
		c.Check(awaitErr(c, stopCh) == nil, "final stop")
	})
}

func TestScreencastFlow(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p := newScreenPeer(t)
		d, err := Connect(strings.TrimPrefix(p.server.URL, "http://"), false)
		c.Check(err == nil, "connect: %v", err)
		if err != nil {
			return
		}
		defer d.Close()

		startCh := make(chan startResult, 1)
		go func() {
			s, err := d.StartScreencast()
			startCh <- startResult{s, err}
		}()
		m := expectCmd(c, p, "Page.startScreencast")
		if m == nil {
			return
		}
		p.reply(t, m, Params{})
		res := awaitStart(c, startCh)
		c.Check(res.err == nil && res.s != nil, "start: %v", res.err)
		if res.err != nil || res.s == nil {
			return
		}
		s := res.s

		// slow consumer: frames replace each other in the mailbox and every
		// one of them (including the replaced ones) is acknowledged
		for i := 1; i <= 4; i++ {
			p.event(t, "Page.screencastFrame", Params{
				"sessionId": i,
				"data":      b64(fmt.Sprintf("frame-%d", i)),
				"metadata":  Params{"n": float64(i)},
			})
			a := expectAck(c, p, i)
			if a == nil {
				return
			}
			p.reply(t, a, Params{})
		}
		f, ok := recvFrame(c, s)
		c.Check(ok && f.SessionID == 4 && string(f.Data) == "frame-4", "mailbox keeps latest: %+v", f)
		c.Check(f.Metadata.Int("n") == 4, "mailbox metadata: %v", f.Metadata)
		expectNoFrame(c, s, 150*time.Millisecond) // replaced frames are not queued

		// the user can replace the event callback; the stream keeps working
		first := make(chan struct{}, 4)
		second := make(chan struct{}, 4)
		d.CallbackEvent("Page.screencastFrame", func(v Params) { first <- struct{}{} })
		p.event(t, "Page.screencastFrame", Params{"sessionId": 5, "data": b64("frame-5")})
		a := expectAck(c, p, 5)
		if a == nil {
			return
		}
		p.reply(t, a, Params{})
		waitSignal(c, first, "first callback")
		f, ok = recvFrame(c, s)
		c.Check(ok && f.SessionID == 5, "frame 5: %+v", f)

		d.CallbackEvent("Page.screencastFrame", func(v Params) { second <- struct{}{} })
		p.event(t, "Page.screencastFrame", Params{"sessionId": 6, "data": b64("frame-6")})
		a = expectAck(c, p, 6)
		if a == nil {
			return
		}
		p.reply(t, a, Params{})
		waitSignal(c, second, "second callback")
		select {
		case <-first:
			c.Check(false, "replaced callback still invoked")
		default:
		}
		f, ok = recvFrame(c, s)
		c.Check(ok && f.SessionID == 6, "frame 6: %+v", f)

		// ordinary commands keep working while streaming
		type evalResult struct {
			v   interface{}
			err error
		}
		evalCh := make(chan evalResult, 1)
		go func() {
			v, err := d.Evaluate("1+1")
			evalCh <- evalResult{v, err}
		}()
		em := expectCmd(c, p, "Runtime.evaluate")
		if em == nil {
			return
		}
		p.reply(t, em, Params{"result": Params{"type": "number", "value": 2.0}})
		eres := <-evalCh
		c.Check(eres.err == nil && eres.v == 2.0, "evaluate: %v %v", eres.v, eres.err)

		mouseCh := make(chan error, 1)
		go func() { mouseCh <- d.MouseEvent(MousePress, 10, 20, LeftButton()) }()
		mm := expectCmd(c, p, "Input.dispatchMouseEvent")
		if mm == nil {
			return
		}
		mp, _ := mm["params"].(map[string]interface{})
		c.Check(mp["type"] == "mousePressed" && mp["x"] == 10.0 && mp["y"] == 20.0 && mp["button"] == "left",
			"mouse event params: %v", mp)
		p.reply(t, mm, Params{})
		c.Check(<-mouseCh == nil, "mouse event")

		// frames are acknowledged even if nobody reads Frames
		p.event(t, "Page.screencastFrame", Params{"sessionId": 7, "data": b64("frame-7")})
		a = expectAck(c, p, 7)
		if a == nil {
			return
		}
		p.reply(t, a, Params{})

		// clean stop: mailbox cleared, Frames closed
		stopCh := make(chan error, 1)
		go func() { stopCh <- s.Stop() }()
		sm := expectCmd(c, p, "Page.stopScreencast")
		if sm == nil {
			return
		}
		p.reply(t, sm, Params{})
		c.Check(awaitErr(c, stopCh) == nil, "stop")
		_, open := <-s.Frames()
		c.Check(!open, "frames channel closed after stop")
	})
}

func TestScreencastStop(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		p := newScreenPeer(t)
		d, err := Connect(strings.TrimPrefix(p.server.URL, "http://"), false)
		c.Check(err == nil, "connect: %v", err)
		if err != nil {
			return
		}
		defer d.Close()

		startCh := make(chan startResult, 1)
		go func() {
			s, err := d.StartScreencast()
			startCh <- startResult{s, err}
		}()
		m := expectCmd(c, p, "Page.startScreencast")
		if m == nil {
			return
		}
		p.reply(t, m, Params{})
		res := awaitStart(c, startCh)
		c.Check(res.err == nil && res.s != nil, "start: %v", res.err)
		if res.err != nil || res.s == nil {
			return
		}
		s := res.s

		p.event(t, "Page.screencastFrame", Params{"sessionId": 1, "data": b64("frame-1")})
		a := expectAck(c, p, 1)
		if a == nil {
			return
		}
		p.reply(t, a, Params{})
		f, ok := recvFrame(c, s)
		c.Check(ok && f.SessionID == 1, "frame 1: %+v", f)

		// concurrent stops share a single stop command and result
		stopCh := make(chan error, 2)
		go func() { stopCh <- s.Stop() }()
		go func() { stopCh <- s.Stop() }()
		sm := expectCmd(c, p, "Page.stopScreencast")
		if sm == nil {
			return
		}

		// while stopping, a new start is rejected without commands
		s2, err := d.StartScreencast()
		c.Check(err == ErrorScreencastActive && s2 == nil, "start while stopping: %v", err)
		expectNoCmd(c, p, 150*time.Millisecond)

		// a frame arriving before the stop reply is acked but not delivered
		p.event(t, "Page.screencastFrame", Params{"sessionId": 2, "data": b64("frame-2")})
		a2 := expectAck(c, p, 2)
		if a2 == nil {
			return
		}
		p.reply(t, a2, Params{})
		expectNoFrame(c, s, 150*time.Millisecond)

		p.reply(t, sm, Params{})
		e1 := awaitErr(c, stopCh)
		e2 := awaitErr(c, stopCh)
		c.Check(e1 == nil && e2 == nil, "concurrent stop results: %v %v", e1, e2)
		expectNoCmd(c, p, 150*time.Millisecond) // no second stop command

		_, open := <-s.Frames()
		c.Check(!open, "frames channel closed after stop")

		// stop after a successful stop returns immediately, without commands
		c.Check(s.Stop() == nil, "stop after stop")
		expectNoCmd(c, p, 150*time.Millisecond)

		// restart after a clean stop
		go func() {
			s, err := d.StartScreencast()
			startCh <- startResult{s, err}
		}()
		m = expectCmd(c, p, "Page.startScreencast")
		if m == nil {
			return
		}
		p.reply(t, m, Params{})
		res = awaitStart(c, startCh)
		c.Check(res.err == nil && res.s != nil, "restart: %v", res.err)
		if res.err != nil || res.s == nil {
			return
		}
		s = res.s

		p.event(t, "Page.screencastFrame", Params{"sessionId": 3, "data": b64("frame-3")})
		a = expectAck(c, p, 3)
		if a == nil {
			return
		}
		p.reply(t, a, Params{})
		f, ok = recvFrame(c, s)
		c.Check(ok && f.SessionID == 3, "frame 3: %+v", f)

		// stop failing with a CDP error keeps the stream; error is preserved
		go func() { stopCh <- s.Stop() }()
		sm = expectCmd(c, p, "Page.stopScreencast")
		if sm == nil {
			return
		}
		replyErr(t, p, sm, -32000, "cannot stop now")
		stopErr := awaitErr(c, stopCh)
		rerr, isRespErr := stopErr.(*ResponseError)
		c.Check(isRespErr && rerr.Code == -32000 && rerr.Message == "cannot stop now",
			"stop error: %#v", stopErr)

		// the stream is still alive
		p.event(t, "Page.screencastFrame", Params{"sessionId": 4, "data": b64("frame-4")})
		a = expectAck(c, p, 4)
		if a == nil {
			return
		}
		p.reply(t, a, Params{})
		f, ok = recvFrame(c, s)
		c.Check(ok && f.SessionID == 4 && string(f.Data) == "frame-4", "frame after failed stop: %+v", f)

		// the stop can be retried and succeeds
		go func() { stopCh <- s.Stop() }()
		sm = expectCmd(c, p, "Page.stopScreencast")
		if sm == nil {
			return
		}
		p.reply(t, sm, Params{})
		c.Check(awaitErr(c, stopCh) == nil, "retried stop")
		_, open = <-s.Frames()
		c.Check(!open, "frames channel closed after retried stop")

		// an ack failure closes the stream and forbids new streams
		go func() {
			s, err := d.StartScreencast()
			startCh <- startResult{s, err}
		}()
		m = expectCmd(c, p, "Page.startScreencast")
		if m == nil {
			return
		}
		p.reply(t, m, Params{})
		res = awaitStart(c, startCh)
		c.Check(res.err == nil && res.s != nil, "restart: %v", res.err)
		if res.err != nil || res.s == nil {
			return
		}
		s = res.s

		p.event(t, "Page.screencastFrame", Params{"sessionId": 5, "data": b64("frame-5")})
		a = expectAck(c, p, 5)
		if a == nil {
			return
		}
		replyErr(t, p, a, -32602, "invalid session")

		c.Check(awaitClosed(c, s), "frames channel closed after ack failure")
		serr := s.Err()
		c.Check(serr != nil, "stream error recorded")
		rerr, isRespErr = serr.(*ResponseError)
		c.Check(isRespErr && rerr.Code == -32602 && rerr.Message == "invalid session",
			"stream error: %#v", serr)

		// no new stream can be started on this connection
		s3, err := d.StartScreencast()
		c.Check(err == serr && s3 == nil, "start after ack failure: %v", err)
		expectNoCmd(c, p, 150*time.Millisecond)

		// Stop on the failed stream reports the error
		c.Check(s.Stop() == serr, "stop of failed stream")
	})
}
