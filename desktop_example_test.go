package vnc

import (
	"context"
	"net"
	"time"

	"github.com/kward/go-vnc/buttons"
)

// ExampleClientConn_SetDesktopSize arranges the server's monitors into two
// side-by-side screens, waits for the server to confirm the new layout, and
// then clicks a control positioned on the second screen.
func ExampleClientConn_SetDesktopSize() {
	// Establish TCP connection.
	nc, err := net.DialTimeout("tcp", "127.0.0.1:5900", 10*time.Second)
	if err != nil {
		panic(err)
	}

	cfg := NewClientConfig("somepass")
	cfg.ServerMessageCh = make(chan ServerMessage, 16)
	vc, err := Connect(context.Background(), nc, cfg)
	if err != nil {
		panic(err)
	}
	go vc.ListenAndHandle()

	// Declare support for both desktop pseudo-encodings before asking the
	// server for an update.
	if err := vc.SetEncodings(Encodings{
		&RawEncoding{},
		&DesktopSizePseudoEncoding{},
		&ExtendedDesktopSizePseudoEncoding{},
	}); err != nil {
		panic(err)
	}
	if err := vc.FramebufferUpdateRequest(false, 0, 0, vc.FramebufferWidth(), vc.FramebufferHeight()); err != nil {
		panic(err)
	}

	// The server only advertises support by sending an ExtendedDesktopSize
	// rectangle; wait until the topology is known.
	for {
		if _, ok := vc.CurrentDesktopLayout(); ok {
			break
		}
		<-cfg.ServerMessageCh
	}

	// Ask for two 1920x1080 screens side by side.
	screens := []Screen{
		{ID: 1, X: 0, Y: 0, Width: 1920, Height: 1080},
		{ID: 2, X: 1920, Y: 0, Width: 1920, Height: 1080},
	}
	if err := vc.SetDesktopSize(3840, 1080, screens); err != nil {
		panic(err)
	}

	// The request does not take effect immediately. Wait for a server-initiated
	// or other-client update that confirms the 3840x1080 layout.
	var layout DesktopLayout
	for {
		msg := <-cfg.ServerMessageCh
		update, ok := msg.(*FramebufferUpdate)
		if !ok {
			continue
		}
		confirmed := false
		for _, rect := range update.Rects {
			if ext, ok := rect.Enc.(*ExtendedDesktopSizePseudoEncoding); ok {
				if ext.Reason != 1 || ext.Result == 0 {
					confirmed = true
				}
			}
		}
		if confirmed {
			layout, _ = vc.CurrentDesktopLayout()
			if layout.Width == 3840 && layout.Height == 1080 {
				break
			}
		}
	}

	// Click a control in the top-left corner of the second screen. Screen
	// coordinates are translated to desktop coordinates by the library.
	second := layout.Screens[1]
	vc.PointerEventOnScreen(second.ID, buttons.Left, 40, 40)
	vc.PointerEventOnScreen(second.ID, buttons.None, 40, 40)

	// Close VNC connection.
	vc.Close()
}
