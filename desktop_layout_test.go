package vnc

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"net"
	"reflect"
	"sync"
	"testing"
	"time"

	"github.com/kward/go-vnc/buttons"
	"github.com/kward/go-vnc/encodings"
	"github.com/kward/go-vnc/task_support/taskcheck"
)

// layoutConn is a controllable in-memory net.Conn. Server-to-client bytes are
// staged in "in"; client-to-server bytes are captured in "out". The two sides
// are independent so the client can be driven while its requests are
// inspected.
type layoutConn struct {
	mu  sync.Mutex
	in  bytes.Buffer
	out bytes.Buffer

	closed bool
}

func (c *layoutConn) Read(b []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.in.Len() == 0 {
		return 0, io.EOF
	}
	return c.in.Read(b)
}

func (c *layoutConn) Write(b []byte) (int, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return 0, errors.New("layoutConn: closed")
	}
	return c.out.Write(b)
}

func (c *layoutConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.closed = true
	return nil
}
func (c *layoutConn) LocalAddr() net.Addr                { return nil }
func (c *layoutConn) RemoteAddr() net.Addr               { return nil }
func (c *layoutConn) SetDeadline(t time.Time) error      { return nil }
func (c *layoutConn) SetReadDeadline(t time.Time) error  { return nil }
func (c *layoutConn) SetWriteDeadline(t time.Time) error { return nil }

func (c *layoutConn) feed(b []byte) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.in.Write(b)
}

func (c *layoutConn) takeOut() []byte {
	c.mu.Lock()
	defer c.mu.Unlock()
	b := append([]byte(nil), c.out.Bytes()...)
	c.out.Reset()
	return b
}

func newDesktopConn() (*ClientConn, *layoutConn) {
	lc := &layoutConn{}
	c := NewClientConn(lc, &ClientConfig{})
	// A zero pixel format makes a zero-sized Raw rectangle decode to zero
	// bytes, which lets the mixed-update tests reach validation.
	c.pixelFormat = PixelFormat{}
	if err := c.SetEncodings(Encodings{
		&RawEncoding{},
		&DesktopSizePseudoEncoding{},
		&ExtendedDesktopSizePseudoEncoding{},
	}); err != nil {
		panic(err)
	}
	lc.takeOut()
	return c, lc
}

func put16(b *bytes.Buffer, v uint16) { binary.Write(b, binary.BigEndian, v) }
func put32(b *bytes.Buffer, v uint32) { binary.Write(b, binary.BigEndian, v) }
func putScreen(b *bytes.Buffer, s Screen) {
	put32(b, s.ID)
	put16(b, s.X)
	put16(b, s.Y)
	put16(b, s.Width)
	put16(b, s.Height)
	put32(b, s.Flags)
}

func rectHeader(x, y, w, h uint16, enc int32) *bytes.Buffer {
	b := &bytes.Buffer{}
	put16(b, x)
	put16(b, y)
	put16(b, w)
	put16(b, h)
	put32(b, uint32(enc))
	return b
}

// extendedRect builds one ExtendedDesktopSize rectangle. The rectangle header
// carries reason/result and the framebuffer size; the body carries screens.
func extendedRect(reason, result, width, height uint16, screens []Screen) []byte {
	b := rectHeader(reason, result, width, height, int32(encodings.ExtendedDesktopSizePseudo))
	b.WriteByte(byte(len(screens)))
	b.Write([]byte{0, 0, 0})
	for _, s := range screens {
		putScreen(b, s)
	}
	return b.Bytes()
}

func legacyRect(width, height uint16) []byte {
	return rectHeader(0, 0, width, height, int32(encodings.DesktopSizePseudo)).Bytes()
}

// truncatedExtendedRect claims two screens but carries only one, so reading it
// fails partway through its screen table.
func truncatedExtendedRect(width, height uint16) []byte {
	screens := []Screen{{ID: 4, X: 0, Y: 0, Width: 100, Height: 100}}
	b := rectHeader(0, 0, width, height, int32(encodings.ExtendedDesktopSizePseudo))
	b.WriteByte(2) // claims two screens
	b.Write([]byte{0, 0, 0})
	putScreen(b, screens[0])
	return b.Bytes()
}

func rawRect() []byte {
	return rectHeader(0, 0, 1, 1, int32(encodings.Raw)).Bytes()
}

// updateBody builds a FramebufferUpdate body: pad(1) + num_rects(2) + rects.
func updateBody(rects ...[]byte) []byte {
	b := &bytes.Buffer{}
	b.Write([]byte{0})
	put16(b, uint16(len(rects)))
	for _, r := range rects {
		b.Write(r)
	}
	return b.Bytes()
}

// updateMessage prefixes the message type, as ListenAndHandle expects.
func updateMessage(rects ...[]byte) []byte {
	return append([]byte{byte(0)}, updateBody(rects...)...)
}

func readUpdate(c *ClientConn) (ServerMessage, error) {
	msg, err := (&FramebufferUpdate{}).Read(c)
	return msg, err
}

func twoScreens() []Screen {
	return []Screen{
		{ID: 10, X: 0, Y: 0, Width: 800, Height: 600},
		{ID: 11, X: 800, Y: 0, Width: 800, Height: 600, Flags: 0xF00DF00D},
	}
}

// receiveExtended drives the real receive path directly.
func receiveExtended(c *ClientConn, reason, result, width, height uint16, screens []Screen) (ServerMessage, error) {
	c.c.(*layoutConn).feed(updateBody(extendedRect(reason, result, width, height, screens)))
	return readUpdate(c)
}

//-----------------------------------------------------------------------------
// TestDesktopLayoutWire exercises encoding declaration, parsing of the wire
// rectangle, and the set-desktop-size request.

func TestDesktopLayoutWire(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		tc, lc := newDesktopConn()

		// The new pseudo-encoding is declared through SetEncodings.
		err := tc.SetEncodings(Encodings{&RawEncoding{}, &ExtendedDesktopSizePseudoEncoding{}})
		c.Check(err == nil, "SetEncodings failed: %v", err)
		out := lc.takeOut()
		c.Check(len(out) == 12, "SetEncodings length = %d, want 12", len(out))
		c.Check(out[0] == byte(2), "message type = %d, want 2", out[0])
		hasExtended := false
		for i := 4; i+4 <= len(out); i += 4 {
			if int32(binary.BigEndian.Uint32(out[i:i+4])) == int32(-308) {
				hasExtended = true
			}
		}
		c.Check(hasExtended, "SetEncodings did not declare -308")
		enc, ok := tc.Encodable(encodings.ExtendedDesktopSizePseudo)
		c.Check(ok && enc != nil, "Encodable(-308) = %v, %v", enc, ok)
		if ok {
			c.Check(enc.Type() == encodings.ExtendedDesktopSizePseudo, "encoding type = %v", enc.Type())
			c.Check(reflect.TypeOf(enc) == reflect.TypeOf(&ExtendedDesktopSizePseudoEncoding{}),
				"encodable type = %T", enc)
		}

		// A complete extended rectangle is decoded and becomes the state.
		screens := twoScreens()
		msg, err := receiveExtended(tc, 0, 0, 1600, 600, screens)
		c.Check(err == nil, "reading extended update: %v", err)
		fu, isFU := msg.(*FramebufferUpdate)
		c.Check(isFU, "message type = %T, want *FramebufferUpdate", msg)
		if isFU {
			c.Check(fu.NumRect == 1 && len(fu.Rects) == 1, "num rects = %d/%d", fu.NumRect, len(fu.Rects))
			ext, ok := fu.Rects[0].Enc.(*ExtendedDesktopSizePseudoEncoding)
			c.Check(ok, "rect encoding = %T", fu.Rects[0].Enc)
			if ok {
				c.Check(ext.Reason == 0 && ext.Result == 0, "reason/result = %d/%d, want 0/0", ext.Reason, ext.Result)
				c.Check(ext.Layout.Width == 1600 && ext.Layout.Height == 600,
					"encoding layout = %dx%d", ext.Layout.Width, ext.Layout.Height)
				c.Check(len(ext.Layout.Screens) == 2, "encoding screens = %d", len(ext.Layout.Screens))
				if len(ext.Layout.Screens) == 2 {
					c.Check(ext.Layout.Screens[1].ID == 11, "encoding screen[1].ID = %d", ext.Layout.Screens[1].ID)
					c.Check(ext.Layout.Screens[1].Flags == 0xF00DF00D,
						"encoding screen[1].Flags = %#x", ext.Layout.Screens[1].Flags)
				}
			}
		}

		layout, known := tc.CurrentDesktopLayout()
		c.Check(known, "topology not known after complete extended rectangle")
		c.Check(layout.Width == 1600 && layout.Height == 600, "layout size = %dx%d", layout.Width, layout.Height)
		c.Check(tc.FramebufferWidth() == 1600 && tc.FramebufferHeight() == 600,
			"framebuffer = %dx%d", tc.FramebufferWidth(), tc.FramebufferHeight())
		c.Check(len(layout.Screens) == 2, "screens = %d, want 2", len(layout.Screens))
		if len(layout.Screens) == 2 {
			c.Check(layout.Screens[0].ID == 10 && layout.Screens[0].X == 0 && layout.Screens[0].Y == 0 &&
				layout.Screens[0].Width == 800 && layout.Screens[0].Height == 600,
				"screen[0] = %+v", layout.Screens[0])
			c.Check(layout.Screens[1].ID == 11 && layout.Screens[1].X == 800 && layout.Screens[1].Y == 0 &&
				layout.Screens[1].Width == 800 && layout.Screens[1].Height == 600,
				"screen[1] = %+v", layout.Screens[1])
			c.Check(layout.Screens[1].Flags == 0xF00DF00D, "flags = %#x, want 0xF00DF00D", layout.Screens[1].Flags)
			c.Check(layout.Screens[1].ID == uint32(11), "ID not preserved")
		}

		// Reason and result are preserved verbatim even for reasons whose
		// result is not meaningful.
		msg, err = receiveExtended(tc, 2, 7, 1024, 768, []Screen{
			{ID: 0x80000000, X: 0, Y: 0, Width: 1024, Height: 768, Flags: 0x1},
		})
		c.Check(err == nil, "reading reason=2 update: %v", err)
		if fu, ok := msg.(*FramebufferUpdate); ok {
			ext, ok := fu.Rects[0].Enc.(*ExtendedDesktopSizePseudoEncoding)
			c.Check(ok, "reason=2 rect encoding = %T", fu.Rects[0].Enc)
			if ok {
				c.Check(ext.Reason == 2, "reason = %d, want 2", ext.Reason)
				c.Check(ext.Result == 7, "result = %d, want 7", ext.Result)
			}
		}
		layout, known = tc.CurrentDesktopLayout()
		c.Check(known, "reason=2 update did not publish topology")
		c.Check(layout.Width == 1024 && layout.Height == 768, "reason=2 size = %dx%d", layout.Width, layout.Height)
		c.Check(len(layout.Screens) == 1 && layout.Screens[0].ID == 0x80000000,
			"reason=2 screens = %+v", layout.Screens)

		// SetDesktopSize writes a type-251 request with the screens verbatim.
		reqScreens := []Screen{
			{ID: 0xDEADBEEF, X: 0, Y: 0, Width: 1920, Height: 1080, Flags: 0x800000AA},
			{ID: 42, X: 1920, Y: 0, Width: 1280, Height: 720, Flags: 0x01},
		}
		lc.takeOut()
		err = tc.SetDesktopSize(3200, 1080, reqScreens)
		c.Check(err == nil, "SetDesktopSize failed: %v", err)
		out = lc.takeOut()
		c.Check(len(out) == 8+2*16, "SetDesktopSize length = %d, want %d", len(out), 8+2*16)
		c.Check(out[0] == 251, "SetDesktopSize message type = %d, want 251", out[0])
		c.Check(out[1] == 0, "SetDesktopSize padding = %d", out[1])
		c.Check(binary.BigEndian.Uint16(out[2:4]) == 3200, "width = %d", binary.BigEndian.Uint16(out[2:4]))
		c.Check(binary.BigEndian.Uint16(out[4:6]) == 1080, "height = %d", binary.BigEndian.Uint16(out[4:6]))
		c.Check(out[6] == 2, "screen count = %d, want 2", out[6])
		c.Check(out[7] == 0, "screen padding = %d", out[7])
		for i, s := range reqScreens {
			off := 8 + i*16
			c.Check(binary.BigEndian.Uint32(out[off:off+4]) == s.ID, "screen[%d].ID = %#x", i, binary.BigEndian.Uint32(out[off:off+4]))
			c.Check(binary.BigEndian.Uint16(out[off+4:off+6]) == s.X, "screen[%d].X = %d", i, binary.BigEndian.Uint16(out[off+4:off+6]))
			c.Check(binary.BigEndian.Uint16(out[off+6:off+8]) == s.Y, "screen[%d].Y = %d", i, binary.BigEndian.Uint16(out[off+6:off+8]))
			c.Check(binary.BigEndian.Uint16(out[off+8:off+10]) == s.Width, "screen[%d].Width = %d", i, binary.BigEndian.Uint16(out[off+8:off+10]))
			c.Check(binary.BigEndian.Uint16(out[off+10:off+12]) == s.Height, "screen[%d].Height = %d", i, binary.BigEndian.Uint16(out[off+10:off+12]))
			c.Check(binary.BigEndian.Uint32(out[off+12:off+16]) == s.Flags, "screen[%d].Flags = %#x", i, binary.BigEndian.Uint32(out[off+12:off+16]))
		}

		// A successful write does not change the current dimensions.
		layout, known = tc.CurrentDesktopLayout()
		c.Check(known && layout.Width == 1024 && layout.Height == 768,
			"SetDesktopSize changed state to %dx%d (known=%v)", layout.Width, layout.Height, known)
		c.Check(tc.FramebufferWidth() == 1024 && tc.FramebufferHeight() == 768,
			"SetDesktopSize changed framebuffer to %dx%d", tc.FramebufferWidth(), tc.FramebufferHeight())

		// A request with an invalid layout is refused before writing.
		lc.takeOut()
		err = tc.SetDesktopSize(100, 100, []Screen{{ID: 1, X: 0, Y: 0, Width: 200, Height: 50}})
		c.Check(err != nil, "SetDesktopSize accepted a screen outside the framebuffer")
		c.Check(len(lc.takeOut()) == 0, "SetDesktopSize wrote after validation failure")
	})
}

//-----------------------------------------------------------------------------
// TestDesktopLayoutState exercises publication, failure receipts, mixed and
// truncated updates, legacy resizing, and concurrent snapshots.

func TestDesktopLayoutState(t *testing.T) {
	SetSettle(0) // Disable UI settling for the input calls in this group.
	taskcheck.Run(t, func(c *taskcheck.C) {
		tc, lc := newDesktopConn()

		// Nothing is published before a complete extended rectangle arrives.
		_, known := tc.CurrentDesktopLayout()
		c.Check(!known, "topology known before any extended rectangle")
		c.Check(tc.SetDesktopSize(800, 600, []Screen{{ID: 1, Width: 800, Height: 600}}) != nil,
			"SetDesktopSize allowed before a complete extended rectangle")

		// Establish a baseline layout.
		msg, err := receiveExtended(tc, 0, 0, 1600, 600, twoScreens())
		c.Check(err == nil, "baseline update: %v", err)
		base, known := tc.CurrentDesktopLayout()
		c.Check(known && base.Width == 1600 && len(base.Screens) == 2, "baseline layout = %+v known=%v", base, known)
		c.Check(len(lc.takeOut()) == 0, "a received extended update triggered a client message")

		// A failure receipt for our own request is delivered on the message
		// channel, but its undefined geometry does not overwrite state.
		cfg := &ClientConfig{
			ServerMessageCh: make(chan ServerMessage, 1),
			ServerMessages: []ServerMessage{
				&FramebufferUpdate{},
				&SetColorMapEntries{},
				&Bell{},
				&ServerCutText{},
			},
		}
		msgTC, msgLC := newDesktopConn()
		msgTC.config = cfg
		// First confirm support so the failure has a well-defined layout.
		msgLC.feed(updateBody(extendedRect(0, 0, 1600, 600, twoScreens())))
		if _, err := readUpdate(msgTC); err != nil {
			c.Check(false, "support update failed: %v", err)
		}
		done := make(chan struct{})
		go func() {
			_ = msgTC.ListenAndHandle()
			close(done)
		}()
		garbage := []Screen{
			{ID: 7, X: 0, Y: 0, Width: 300, Height: 200},
			{ID: 7, X: 300, Y: 0, Width: 300, Height: 200},
		}
		msgLC.feed(updateMessage(extendedRect(1, 4, 600, 200, garbage)))
		select {
		case got := <-cfg.ServerMessageCh:
			fu, ok := got.(*FramebufferUpdate)
			c.Check(ok, "channel message type = %T", got)
			if ok {
				c.Check(len(fu.Rects) == 1, "channel rects = %d", len(fu.Rects))
				ext, ok := fu.Rects[0].Enc.(*ExtendedDesktopSizePseudoEncoding)
				c.Check(ok, "channel encoding = %T", fu.Rects[0].Enc)
				if ok {
					c.Check(ext.Reason == 1, "failure reason = %d, want 1", ext.Reason)
					c.Check(ext.Result == 4, "failure result = %d, want 4", ext.Result)
					c.Check(len(ext.Layout.Screens) == 2, "failure layout screens = %d", len(ext.Layout.Screens))
				}
			}
		case <-time.After(5 * time.Second):
			c.Check(false, "failure receipt was not delivered on the server message channel")
		}
		after, known := msgTC.CurrentDesktopLayout()
		c.Check(known && after.Width == 1600 && after.Height == 600,
			"failure receipt changed layout to %dx%d known=%v", after.Width, after.Height, known)
		c.Check(len(after.Screens) == 2 && after.Screens[1].ID == 11,
			"failure receipt changed screens to %+v", after.Screens)
		<-done

		// Several extended rectangles are applied in wire order.
		first := []Screen{{ID: 1, X: 0, Y: 0, Width: 800, Height: 600}}
		second := []Screen{{ID: 2, X: 0, Y: 0, Width: 1024, Height: 768}}
		lc.feed(updateBody(
			extendedRect(0, 0, 800, 600, first),
			extendedRect(0, 0, 1024, 768, second),
		))
		msg, err = readUpdate(tc)
		c.Check(err == nil, "multiple extended rectangles: %v", err)
		fu, ok := msg.(*FramebufferUpdate)
		c.Check(ok && len(fu.Rects) == 2, "multiple rects = %v", msg)
		if ok && len(fu.Rects) == 2 {
			e0, _ := fu.Rects[0].Enc.(*ExtendedDesktopSizePseudoEncoding)
			e1, _ := fu.Rects[1].Enc.(*ExtendedDesktopSizePseudoEncoding)
			c.Check(e0 != nil && e0.Layout.Width == 800, "first extended rect layout = %+v", e0)
			c.Check(e1 != nil && e1.Layout.Width == 1024, "second extended rect layout = %+v", e1)
		}
		layout, known := tc.CurrentDesktopLayout()
		c.Check(known && layout.Width == 1024 && layout.Height == 768 && len(layout.Screens) == 1 && layout.Screens[0].ID == 2,
			"layout after ordered update = %+v", layout)

		// A valid rectangle followed by a truncated one publishes nothing.
		lc.feed(updateBody(
			extendedRect(0, 0, 640, 480, []Screen{{ID: 3, Width: 640, Height: 480}}),
			truncatedExtendedRect(640, 480),
		))
		_, err = readUpdate(tc)
		c.Check(err != nil, "truncated second rectangle did not fail: %v", err)
		layout, known = tc.CurrentDesktopLayout()
		c.Check(known && layout.Width == 1024 && layout.Screens[0].ID == 2,
			"truncated update published layout %+v (known=%v)", layout, known)

		// A fresh connection does not gain support from an invalid update.
		badTC, badLC := newDesktopConn()
		badLC.feed(updateBody(
			extendedRect(0, 0, 100, 100, []Screen{{ID: 1, Width: 100, Height: 100}}),
			legacyRect(100, 100),
		))
		_, err = readUpdate(badTC)
		c.Check(err != nil, "extended+legacy update accepted: %v", err)
		c.Check(len(badLC.takeOut()) == 0, "invalid update wrote to the server")
		_, known = badTC.CurrentDesktopLayout()
		c.Check(!known, "invalid extended+legacy update published topology")
		c.Check(badTC.SetDesktopSize(100, 100, []Screen{{ID: 1, Width: 100, Height: 100}}) != nil,
			"invalid extended+legacy update granted SetDesktopSize support")

		// An extended update mixed with a Raw pixel rectangle is rejected
		// and publishes neither support nor layout.
		badLC.feed(updateBody(
			extendedRect(0, 0, 100, 100, []Screen{{ID: 1, Width: 100, Height: 100}}),
			rawRect(),
		))
		_, err = readUpdate(badTC)
		c.Check(err != nil, "extended+raw update accepted: %v", err)
		_, known = badTC.CurrentDesktopLayout()
		c.Check(!known, "extended+raw update published topology")
		c.Check(badTC.SetDesktopSize(100, 100, []Screen{{ID: 1, Width: 100, Height: 100}}) != nil,
			"extended+raw update granted SetDesktopSize support")

		// A layout with duplicate IDs is invalid and publishes nothing.
		layoutBadTC, layoutBadLC := newDesktopConn()
		layoutBadLC.feed(updateBody(extendedRect(0, 0, 200, 100, []Screen{
			{ID: 1, X: 0, Y: 0, Width: 200, Height: 100},
			{ID: 1, X: 0, Y: 0, Width: 200, Height: 100},
		})))
		_, err = readUpdate(layoutBadTC)
		c.Check(err != nil, "duplicate screen IDs accepted: %v", err)
		_, known = layoutBadTC.CurrentDesktopLayout()
		c.Check(!known, "invalid layout published topology")

		// Legacy DesktopSize still resizes, and drops the old topology.
		lc.feed(updateBody(legacyRect(640, 480)))
		_, err = readUpdate(tc)
		c.Check(err == nil, "legacy DesktopSize: %v", err)
		c.Check(tc.FramebufferWidth() == 640 && tc.FramebufferHeight() == 480,
			"legacy size = %dx%d", tc.FramebufferWidth(), tc.FramebufferHeight())
		_, known = tc.CurrentDesktopLayout()
		c.Check(!known, "legacy DesktopSize did not clear the topology")
		// A later extended update restores the topology.
		_, err = receiveExtended(tc, 0, 0, 1600, 600, twoScreens())
		c.Check(err == nil, "extended update after legacy: %v", err)
		_, known = tc.CurrentDesktopLayout()
		c.Check(known, "extended update after legacy did not restore topology")
		// No automatic FramebufferUpdateRequest is appended by any of these.
		c.Check(len(lc.takeOut()) == 0, "desktop updates emitted a client message")

		// Concurrent receiving and snapshotting never observes dimensions
		// from one update and screens from another.
		concTC, concLC := newDesktopConn()
		updateA := extendedRect(0, 0, 200, 100, []Screen{{ID: 7, X: 0, Y: 0, Width: 200, Height: 100}})
		updateB := extendedRect(0, 0, 500, 300, []Screen{
			{ID: 7, X: 0, Y: 0, Width: 200, Height: 100},
			{ID: 9, X: 200, Y: 0, Width: 300, Height: 300},
		})
		concLC.feed(updateBody(updateA))
		if _, err := readUpdate(concTC); err != nil {
			c.Check(false, "concurrent baseline: %v", err)
		}
		var wg sync.WaitGroup
		wg.Add(1)
		readerErr := make(chan error, 1)
		go func() {
			defer wg.Done()
			for i := 0; i < 200; i++ {
				rect := updateA
				if i%2 == 1 {
					rect = updateB
				}
				concLC.feed(updateBody(rect))
				if _, err := readUpdate(concTC); err != nil {
					readerErr <- err
					return
				}
			}
			readerErr <- nil
		}()
		bad := 0
		for i := 0; i < 200; i++ {
			layout, known := concTC.CurrentDesktopLayout()
			if !known {
				c.Check(false, "concurrent snapshot lost topology")
				continue
			}
			coherent := (layout.Width == 200 && layout.Height == 100 &&
				len(layout.Screens) == 1 && layout.Screens[0].ID == 7) ||
				(layout.Width == 500 && layout.Height == 300 &&
					len(layout.Screens) == 2 && layout.Screens[1].ID == 9)
			if !coherent {
				bad++
				c.Check(false, "mixed snapshot: %dx%d screens=%+v", layout.Width, layout.Height, layout.Screens)
			}
			if err := concTC.PointerEventOnScreen(7, buttons.None, 50, 50); err != nil {
				c.Check(false, "concurrent screen input: %v", err)
			}
		}
		wg.Wait()
		if err := <-readerErr; err != nil {
			c.Check(false, "concurrent reader: %v", err)
		}
		c.Check(bad == 0, "%d non-coherent snapshots", bad)
	})
}

//-----------------------------------------------------------------------------
// TestDesktopLayoutInput exercises screen-relative input, snapshots, aliasing,
// legacy invalidation, and per-connection isolation.

func TestDesktopLayoutInput(t *testing.T) {
	SetSettle(0) // Disable UI settling for the input calls in this group.
	taskcheck.Run(t, func(c *taskcheck.C) {
		tc, lc := newDesktopConn()

		// Without a confirmed topology, no input may be sent.
		err := tc.PointerEventOnScreen(10, buttons.Left, 1, 1)
		c.Check(err != nil, "PointerEventOnScreen succeeded before a layout")
		c.Check(len(lc.takeOut()) == 0, "PointerEventOnScreen wrote without a layout")

		_, err = receiveExtended(tc, 0, 0, 1600, 600, twoScreens())
		c.Check(err == nil, "layout update: %v", err)
		lc.takeOut()

		// Coordinates are relative to the confirmed screen and are translated
		// to desktop coordinates through the original PointerEvent path.
		err = tc.PointerEventOnScreen(11, buttons.Left|buttons.Middle, 123, 456)
		c.Check(err == nil, "PointerEventOnScreen failed: %v", err)
		out := lc.takeOut()
		c.Check(len(out) == 6, "pointer length = %d, want 6", len(out))
		c.Check(out[0] == 5, "message type = %d, want 5", out[0])
		c.Check(out[1] == uint8(buttons.Left|buttons.Middle), "button mask = %d", out[1])
		c.Check(binary.BigEndian.Uint16(out[2:4]) == 923, "desktop x = %d, want 923", binary.BigEndian.Uint16(out[2:4]))
		c.Check(binary.BigEndian.Uint16(out[4:6]) == 456, "desktop y = %d, want 456", binary.BigEndian.Uint16(out[4:6]))

		// The last in-bounds pixel of the screen maps correctly.
		err = tc.PointerEventOnScreen(11, buttons.None, 799, 599)
		c.Check(err == nil, "last pixel rejected: %v", err)
		out = lc.takeOut()
		c.Check(binary.BigEndian.Uint16(out[2:4]) == 1599 && binary.BigEndian.Uint16(out[4:6]) == 599,
			"last pixel mapped to (%d,%d)", binary.BigEndian.Uint16(out[2:4]), binary.BigEndian.Uint16(out[4:6]))

		// Out-of-range, unknown ID, and missing topology fail before any write.
		for _, tt := range []struct {
			desc string
			id   uint32
			x, y uint16
		}{
			{"x at width", 11, 800, 0},
			{"y at height", 11, 0, 600},
			{"unknown ID", 99, 0, 0},
		} {
			lc.takeOut()
			err := tc.PointerEventOnScreen(tt.id, buttons.Left, tt.x, tt.y)
			c.Check(err != nil, "%s: expected error", tt.desc)
			c.Check(len(lc.takeOut()) == 0, "%s: wrote before reporting the error", tt.desc)
		}

		// PointerEvent still accepts absolute desktop coordinates.
		err = tc.PointerEvent(buttons.None, 1500, 500)
		c.Check(err == nil, "PointerEvent failed: %v", err)
		out = lc.takeOut()
		c.Check(len(out) == 6 && out[0] == 5, "pointer length/type = %d/%d", len(out), out[0])
		c.Check(binary.BigEndian.Uint16(out[2:4]) == 1500 && binary.BigEndian.Uint16(out[4:6]) == 500,
			"desktop pointer = (%d,%d)", binary.BigEndian.Uint16(out[2:4]), binary.BigEndian.Uint16(out[4:6]))

		// Snapshot slices are independent of the connection's layout.
		snap, known := tc.CurrentDesktopLayout()
		c.Check(known, "topology unexpectedly unknown")
		snap.Screens[0].X = 12345
		snap.Screens[0].Width = 1
		snap.Screens = append(snap.Screens, Screen{ID: 555, Width: 1, Height: 1})
		snap.Width = 1
		snap.Height = 1
		snap2, known := tc.CurrentDesktopLayout()
		c.Check(known && snap2.Width == 1600 && len(snap2.Screens) == 2,
			"snapshot aliased the connection: %+v", snap2)
		c.Check(snap2.Screens[0].X == 0 && snap2.Screens[0].Width == 800,
			"snapshot screen aliased the connection: %+v", snap2.Screens[0])
		// Input still works from the untouched state.
		err = tc.PointerEventOnScreen(11, buttons.Left, 10, 10)
		c.Check(err == nil, "input after snapshot mutation: %v", err)
		lc.takeOut()

		// Screens passed to SetDesktopSize are copied; mutating the caller's
		// slice does not affect the current confirmed layout.
		reqScreens := []Screen{{ID: 11, X: 0, Y: 0, Width: 1600, Height: 600}}
		err = tc.SetDesktopSize(1600, 600, reqScreens)
		c.Check(err == nil, "SetDesktopSize failed: %v", err)
		lc.takeOut()
		reqScreens[0].Width = 1
		reqScreens[0].ID = 12
		snap3, known := tc.CurrentDesktopLayout()
		c.Check(known && snap3.Width == 1600 && len(snap3.Screens) == 2 && snap3.Screens[1].ID == 11,
			"SetDesktopSize input aliased state: %+v", snap3)

		// Legacy DesktopSize invalidates the topology; screen input must
		// report an error before writing anything.
		lc.feed(updateBody(legacyRect(640, 480)))
		_, err = readUpdate(tc)
		c.Check(err == nil, "legacy DesktopSize: %v", err)
		lc.takeOut()
		err = tc.PointerEventOnScreen(11, buttons.Left, 5, 5)
		c.Check(err != nil, "screen input after legacy resize succeeded")
		c.Check(len(lc.takeOut()) == 0, "screen input after legacy resize wrote data")
		_, known = tc.CurrentDesktopLayout()
		c.Check(!known, "legacy resize left the topology known")
		err = tc.PointerEvent(buttons.Left, 320, 240)
		c.Check(err == nil, "desktop pointer after legacy resize: %v", err)
		out = lc.takeOut()
		c.Check(binary.BigEndian.Uint16(out[2:4]) == 320 && binary.BigEndian.Uint16(out[4:6]) == 240,
			"desktop pointer after legacy = (%d,%d)", binary.BigEndian.Uint16(out[2:4]), binary.BigEndian.Uint16(out[4:6]))

		// Two connections keep independent layouts, support, and input.
		tcA, lcA := newDesktopConn()
		_, err = receiveExtended(tcA, 0, 0, 800, 600, []Screen{{ID: 1, X: 0, Y: 0, Width: 800, Height: 600}})
		c.Check(err == nil, "connection A layout: %v", err)
		lcA.takeOut()
		tcB, lcB := newDesktopConn()
		c.Check(tcB.SetDesktopSize(800, 600, []Screen{{ID: 1, Width: 800, Height: 600}}) != nil,
			"connection B SetDesktopSize before support")
		_, knownB := tcB.CurrentDesktopLayout()
		c.Check(!knownB, "connection B topology leaked from A")
		err = tcA.PointerEventOnScreen(1, buttons.Left, 40, 50)
		c.Check(err == nil, "connection A input failed: %v", err)
		outA := lcA.takeOut()
		c.Check(len(outA) == 6 && binary.BigEndian.Uint16(outA[2:4]) == 40 && binary.BigEndian.Uint16(outA[4:6]) == 50,
			"connection A pointer = %x", outA)
		c.Check(len(lcB.takeOut()) == 0, "connection A input leaked to connection B")
		err = tcB.PointerEventOnScreen(1, buttons.Left, 40, 50)
		c.Check(err != nil, "connection B input succeeded without a layout")
		c.Check(len(lcB.takeOut()) == 0, "connection B input wrote without a layout")
	})
}
