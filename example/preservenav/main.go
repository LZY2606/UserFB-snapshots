// Command preservenav demonstrates how to grow the set of observed
// DevTools events while a form replay reaches its confirmation step,
// without losing events that are already buffered.
//
// It uses cdp.SyncPreserve instead of cdp.Sync. Sync would discard the
// buffered events; SyncPreserve merges the existing synchronous group with
// the newly created event clients and keeps delivering all unconsumed
// events in the order the connection received them, including the popup
// and navigation events emitted by the confirmation step.
//
// The example does not run a browser: the RPC connection is replaced by a
// scripted in-memory pipe. Run it with:
//
//	go run ./example/preservenav
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net"
	"sync"
	"time"

	"github.com/mafredri/cdp"
	"github.com/mafredri/cdp/rpcc"
)

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}

func run() error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	local, remote := net.Pipe()
	defer remote.Close()

	peer := &peerConn{conn: remote, enc: json.NewEncoder(remote)}
	go peer.serve()

	conn, err := rpcc.DialContext(ctx, "pipe",
		rpcc.WithDialer(func(context.Context, string) (io.ReadWriteCloser, error) {
			return local, nil
		}),
		rpcc.WithCodec(func(r io.ReadWriter) rpcc.Codec {
			return &jsonCodec{enc: json.NewEncoder(r), dec: json.NewDecoder(r)}
		}),
	)
	if err != nil {
		return err
	}
	defer conn.Close()

	c := cdp.NewClient(conn)

	// The replay starts by observing the document events needed for the
	// first step of the form.
	domContent, err := c.Page.DOMContentEventFired(ctx)
	if err != nil {
		return err
	}
	defer domContent.Close()
	load, err := c.Page.LoadEventFired(ctx)
	if err != nil {
		return err
	}
	defer load.Close()

	// The form replay starts by observing the document events needed for
	// the first step. cdp.Sync creates the first synchronous group; this
	// happens before any event is relevant, so clearing is harmless here.
	if err := cdp.Sync(domContent, load); err != nil {
		return err
	}

	// Enabling the domains can emit events immediately. Those events are
	// buffered by the group and must not be lost when the next step adds
	// more clients.
	if err := c.Page.Enable(ctx, nil); err != nil {
		return err
	}
	peer.event("Page.domContentEventFired", map[string]float64{"timestamp": 1})
	peer.event("Page.loadEventFired", map[string]float64{"timestamp": 2})
	if err := c.Page.Enable(ctx, nil); err != nil { // Receive barrier.
		return err
	}

	// The form replay reaches the confirmation step. Popups and navigation
	// become relevant, so new event clients are created.
	frameNavigated, err := c.Page.FrameNavigated(ctx)
	if err != nil {
		return err
	}
	defer frameNavigated.Close()
	dialogOpening, err := c.Page.JavascriptDialogOpening(ctx)
	if err != nil {
		return err
	}
	defer dialogOpening.Close()
	requestWillBeSent, err := c.Network.RequestWillBeSent(ctx)
	if err != nil {
		return err
	}
	defer requestWillBeSent.Close()

	// All live members of the old group (domContent and load) must be
	// passed together with the new clients. Passing only one of them
	// returns an error and leaves the old group untouched. The generated
	// clients participate directly; no client is rebuilt.
	err = cdp.SyncPreserve(domContent, load, frameNavigated, dialogOpening, requestWillBeSent)
	if err != nil {
		return fmt.Errorf("SyncPreserve: %w", err)
	}

	// The confirmation step runs and its events join the same group.
	peer.event("Page.javascriptDialogOpening", map[string]interface{}{
		"url":               "https://example.test/form",
		"frameId":           "main",
		"message":           "Submit this form?",
		"type":              "confirm",
		"hasBrowserHandler": false,
	})
	peer.event("Page.frameNavigated", map[string]interface{}{
		"frame": map[string]interface{}{
			"id":       "main",
			"loaderId": "loader-2",
			"url":      "https://example.test/form/confirm",
		},
		"type": "Navigation",
	})
	peer.event("Network.requestWillBeSent", map[string]interface{}{
		"requestId":   "request-1",
		"loaderId":    "loader-2",
		"documentURL": "https://example.test/form/confirm",
		"request": map[string]interface{}{
			"url":             "https://example.test/form/confirm",
			"method":          "POST",
			"headers":         map[string]string{},
			"initialPriority": "High",
			"referrerPolicy":  "no-referrer",
		},
		"timestamp": 3,
		"wallTime":  3,
		"initiator": map[string]interface{}{"type": "parser"},
	})
	if err := c.Page.Enable(ctx, nil); err != nil { // Receive barrier.
		return err
	}

	// Drain the merged group in arrival order. Ready identifies the client
	// whose next event can be read without blocking; an event queued
	// behind another client's event does not become ready early.
	streams := []struct {
		name string
		s    rpcc.Stream
	}{
		{"Page.domContentEventFired", domContent},
		{"Page.loadEventFired", load},
		{"Page.javascriptDialogOpening", dialogOpening},
		{"Page.frameNavigated", frameNavigated},
		{"Network.requestWillBeSent", requestWillBeSent},
	}
	remaining := len(streams)
	for remaining > 0 {
		var picked bool
		for _, stream := range streams {
			select {
			case <-stream.s.Ready():
			default:
				continue
			}
			var raw []byte
			if err := stream.s.RecvMsg(&raw); err != nil {
				return err
			}
			fmt.Printf("%s: %s\n", stream.name, raw)
			remaining--
			picked = true
		}
		if !picked {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(5 * time.Millisecond):
			}
		}
	}

	return nil
}

// peerConn is the server side of the in-memory connection. It answers
// requests and emits scripted notifications. Writes are serialized so
// that notifications and replies never interleave bytes.
type peerConn struct {
	mu   sync.Mutex
	conn net.Conn
	enc  *json.Encoder
}

func (p *peerConn) serve() {
	dec := json.NewDecoder(p.conn)
	for {
		var req struct {
			ID uint64 `json:"id"`
		}
		if err := dec.Decode(&req); err != nil {
			return
		}
		p.mu.Lock()
		err := p.enc.Encode(map[string]interface{}{
			"id":     req.ID,
			"result": map[string]interface{}{},
		})
		p.mu.Unlock()
		if err != nil {
			return
		}
	}
}

func (p *peerConn) event(method string, params interface{}) {
	p.mu.Lock()
	defer p.mu.Unlock()
	err := p.enc.Encode(map[string]interface{}{"method": method, "params": params})
	if err != nil {
		log.Printf("emit %s: %v", method, err)
	}
}

type jsonCodec struct {
	enc *json.Encoder
	dec *json.Decoder
}

func (c *jsonCodec) WriteRequest(r *rpcc.Request) error  { return c.enc.Encode(r) }
func (c *jsonCodec) ReadResponse(r *rpcc.Response) error { return c.dec.Decode(r) }
