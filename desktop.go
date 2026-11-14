// Implementation of the RFB desktop resizing extension:
// SetDesktopSize (client-to-server message 251) and the
// ExtendedDesktopSize pseudo-encoding (-308).
//
// The connection keeps a single desktop model: the framebuffer size and the
// screen table are always published together, under a lock, only after a
// complete FramebufferUpdate has been parsed and validated.

package vnc

import (
	"fmt"

	"github.com/kward/go-vnc/buttons"
	"github.com/kward/go-vnc/encoding"
	"github.com/kward/go-vnc/encodings"
	"github.com/kward/go-vnc/logging"
	"github.com/kward/go-vnc/messages"
)

// Screen describes one screen in a desktop layout.
type Screen struct {
	ID     uint32
	X, Y   uint16
	Width  uint16
	Height uint16
	Flags  uint32
}

// DesktopLayout describes the size of the framebuffer and its screens.
type DesktopLayout struct {
	Width   uint16
	Height  uint16
	Screens []Screen
}

// ExtendedDesktopSizePseudoEncoding is the -308 pseudo-encoding. Its result
// keeps the reason and result fields from the rectangle header along with the
// layout that was decoded from the rectangle body.
type ExtendedDesktopSizePseudoEncoding struct {
	Reason uint16
	Result uint16
	Layout DesktopLayout
}

// Verify that interfaces are honored.
var _ Encoding = (*ExtendedDesktopSizePseudoEncoding)(nil)

// Marshal implements the Marshaler interface.
func (e *ExtendedDesktopSizePseudoEncoding) Marshal() ([]byte, error) {
	return []byte{}, nil
}

// Read implements the Encoding interface. It reads the extended desktop
// rectangle body and returns the parsed result. It does not publish any state;
// publication happens once the whole FramebufferUpdate has been validated.
func (*ExtendedDesktopSizePseudoEncoding) Read(c *ClientConn, rect *Rectangle) (Encoding, error) {
	wire, err := encoding.ReadExtendedDesktopSizeWire(c.c)
	if err != nil {
		return nil, fmt.Errorf("unable to read extended desktop size: %s", err)
	}
	layout := DesktopLayout{
		Width:   rect.Width,
		Height:  rect.Height,
		Screens: make([]Screen, len(wire.Screens)),
	}
	for i, s := range wire.Screens {
		layout.Screens[i] = Screen{
			ID:     s.ID,
			X:      s.X,
			Y:      s.Y,
			Width:  s.Width,
			Height: s.Height,
			Flags:  s.Flags,
		}
	}
	return &ExtendedDesktopSizePseudoEncoding{
		Reason: rect.X,
		Result: rect.Y,
		Layout: layout,
	}, nil
}

// String implements the fmt.Stringer interface.
func (e *ExtendedDesktopSizePseudoEncoding) String() string {
	return "ExtendedDesktopSizePseudoEncoding"
}

// Type implements the Encoding interface.
func (*ExtendedDesktopSizePseudoEncoding) Type() encodings.Encoding {
	return encodings.ExtendedDesktopSizePseudo
}

// SetDesktopSize asks the server to resize the desktop. It may only be called
// after a complete ExtendedDesktopSize rectangle has been received. Sending the
// request does not change the current dimensions; only a later server update
// confirms them.
func (c *ClientConn) SetDesktopSize(width, height uint16, screens []Screen) error {
	if logging.V(logging.FnDeclLevel) {
		logging.Infof("ClientConn.%s", logging.FnNameWithArgs("%d, %d, %d screens", width, height, len(screens)))
	}

	c.mu.RLock()
	supported := c.desktopSupported
	c.mu.RUnlock()
	if !supported {
		return NewVNCError("SetDesktopSize: server has not sent a complete ExtendedDesktopSize rectangle")
	}

	// Copy the caller's screens so the request cannot alias their slice.
	screenCopy := make([]Screen, len(screens))
	copy(screenCopy, screens)
	layout := DesktopLayout{Width: width, Height: height, Screens: screenCopy}
	if err := validateDesktopLayout(&layout); err != nil {
		return err
	}

	wireScreens := make([]encoding.ScreenWire, len(screenCopy))
	for i, s := range screenCopy {
		wireScreens[i] = encoding.ScreenWire{
			ID:     s.ID,
			X:      s.X,
			Y:      s.Y,
			Width:  s.Width,
			Height: s.Height,
			Flags:  s.Flags,
		}
	}
	wmsg := encoding.SetDesktopSizeRequestWire{
		MsgType: byte(messages.SetDesktopSize),
		Width:   width,
		Height:  height,
		Screens: wireScreens,
	}
	b, err := wmsg.WireMarshal()
	if err != nil {
		return err
	}
	return c.send(b)
}

// CurrentDesktopLayout returns an independent snapshot of the current desktop
// layout. The boolean reports whether the screen topology is known.
func (c *ClientConn) CurrentDesktopLayout() (DesktopLayout, bool) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if !c.desktopKnown {
		return DesktopLayout{}, false
	}
	return cloneDesktopLayout(c.desktop), true
}

// PointerEventOnScreen sends a pointer event whose coordinates are relative to
// the screen with the given ID. The ID, the current topology, and the bounds
// are checked before anything is written to the server.
func (c *ClientConn) PointerEventOnScreen(id uint32, button buttons.Button, x, y uint16) error {
	c.mu.RLock()
	if !c.desktopKnown {
		c.mu.RUnlock()
		return NewVNCError("PointerEventOnScreen: desktop layout is not known")
	}
	var screen Screen
	found := false
	for _, s := range c.desktop.Screens {
		if s.ID == id {
			screen = s
			found = true
			break
		}
	}
	if !found {
		c.mu.RUnlock()
		return Errorf("PointerEventOnScreen: unknown screen ID %d", id)
	}
	if uint32(x) >= uint32(screen.Width) || uint32(y) >= uint32(screen.Height) {
		c.mu.RUnlock()
		return Errorf("PointerEventOnScreen: point (%d,%d) outside screen %d", x, y, id)
	}
	desktopX := uint32(screen.X) + uint32(x)
	desktopY := uint32(screen.Y) + uint32(y)
	c.mu.RUnlock()

	return c.PointerEvent(button, uint16(desktopX), uint16(desktopY))
}

// pendingDesktopUpdate holds the desktop pseudo-rectangles from one
// FramebufferUpdate in wire order until the whole update is validated.
type pendingDesktopUpdate struct {
	hasExtended bool
	hasLegacy   bool
	hasPixels   bool
	extended    []*ExtendedDesktopSizePseudoEncoding
	legacy      []Rectangle
}

// add records one parsed rectangle. Pixel encodings have non-negative type
// numbers; pseudo-encodings are negative.
func (p *pendingDesktopUpdate) add(rect Rectangle) {
	switch enc := rect.Enc.(type) {
	case *ExtendedDesktopSizePseudoEncoding:
		p.hasExtended = true
		p.extended = append(p.extended, enc)
	case *DesktopSizePseudoEncoding:
		p.hasLegacy = true
		p.legacy = append(p.legacy, rect)
	default:
		if rect.Enc != nil && rect.Enc.Type() >= 0 {
			p.hasPixels = true
		}
	}
}

// extendedSucceeded reports whether an extended rectangle carries a usable
// layout. Only reason 1 has a meaningful result; other reasons are server or
// other-client changes and are applied as-is.
func extendedSucceeded(e *ExtendedDesktopSizePseudoEncoding) bool {
	return e.Reason != 1 || e.Result == 0
}

// validate returns an error if this update's desktop rectangles are not a
// consistent update. Nothing is published here.
func (p *pendingDesktopUpdate) validate() error {
	if !p.hasExtended {
		return nil
	}
	if p.hasLegacy {
		return NewVNCError("FramebufferUpdate: extended desktop update also contains legacy DesktopSize")
	}
	if p.hasPixels {
		return NewVNCError("FramebufferUpdate: extended desktop update contains a pixel rectangle")
	}
	for _, e := range p.extended {
		if !extendedSucceeded(e) {
			continue // Result != 0: geometry is undefined, do not interpret it.
		}
		if err := validateDesktopLayout(&e.Layout); err != nil {
			return err
		}
	}
	return nil
}

// publish applies this update's desktop changes atomically. validate must have
// returned nil.
func (p *pendingDesktopUpdate) publish(c *ClientConn) {
	if !p.hasExtended && !p.hasLegacy {
		return
	}

	c.mu.Lock()
	defer c.mu.Unlock()
	if p.hasExtended {
		c.desktopSupported = true
		for _, e := range p.extended {
			if !extendedSucceeded(e) {
				continue
			}
			c.fbWidth = e.Layout.Width
			c.fbHeight = e.Layout.Height
			c.desktop = cloneDesktopLayout(e.Layout)
			c.desktopKnown = true
		}
		return
	}
	// Legacy DesktopSize changes the size and invalidates the topology.
	for _, rect := range p.legacy {
		c.fbWidth = rect.Width
		c.fbHeight = rect.Height
	}
	c.desktop = DesktopLayout{}
	c.desktopKnown = false
}

func validateDesktopLayout(layout *DesktopLayout) error {
	if layout.Width == 0 || layout.Height == 0 {
		return NewVNCError("desktop layout: framebuffer dimensions must be non-zero")
	}
	if len(layout.Screens) < 1 || len(layout.Screens) > 255 {
		return Errorf("desktop layout: invalid screen count %d", len(layout.Screens))
	}
	ids := make(map[uint32]bool, len(layout.Screens))
	for _, s := range layout.Screens {
		if s.Width == 0 || s.Height == 0 {
			return Errorf("desktop layout: screen %d has zero dimensions", s.ID)
		}
		if ids[s.ID] {
			return Errorf("desktop layout: duplicate screen ID %d", s.ID)
		}
		ids[s.ID] = true
		if uint32(s.X)+uint32(s.Width) > uint32(layout.Width) ||
			uint32(s.Y)+uint32(s.Height) > uint32(layout.Height) {
			return Errorf("desktop layout: screen %d outside framebuffer", s.ID)
		}
	}
	return nil
}

func cloneDesktopLayout(layout DesktopLayout) DesktopLayout {
	out := DesktopLayout{Width: layout.Width, Height: layout.Height}
	if layout.Screens != nil {
		out.Screens = make([]Screen, len(layout.Screens))
		copy(out.Screens, layout.Screens)
	}
	return out
}
