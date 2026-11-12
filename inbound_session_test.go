// Copyright 2026 The mqtt-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package mqtt

import (
	"bytes"
	"context"
	"errors"
	"io"
	"net"
	"sync"
	"testing"
	"time"
)

// recHandler records every delivered message. Messages are cloned on arrival
// so tests can also mutate the Message inside the callback.
type recHandler struct {
	mu   sync.Mutex
	msgs []*Message
	// notify, when set, receives one signal per callback. It lets tests wait
	// for a delivery on a channel fence instead of polling.
	notify chan struct{}
}

func (h *recHandler) Serve(m *Message) {
	h.mu.Lock()
	h.msgs = append(h.msgs, m.clone())
	h.mu.Unlock()
	if h.notify != nil {
		h.notify <- struct{}{}
	}
}

func (h *recHandler) waitCall(t *testing.T, what string) {
	t.Helper()
	if h.notify == nil {
		t.Fatal("handler has no notify channel")
	}
	select {
	case <-h.notify:
	case <-time.After(testTimeout):
		t.Fatalf("timeout waiting for %s", what)
	}
}

func (h *recHandler) calls() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.msgs)
}

func (h *recHandler) payloads() [][]byte {
	h.mu.Lock()
	defer h.mu.Unlock()
	out := make([][]byte, len(h.msgs))
	for i, m := range h.msgs {
		out[i] = append([]byte{}, m.Payload...)
	}
	return out
}

const testTimeout = 5 * time.Second

// readFrame reads one complete MQTT packet from a scripted broker connection.
func readFrame(t *testing.T, conn net.Conn) []byte {
	t.Helper()
	_ = conn.SetReadDeadline(time.Now().Add(testTimeout))
	first := make([]byte, 1)
	if _, err := io.ReadFull(conn, first); err != nil {
		t.Fatalf("reading packet header: %v", err)
	}
	var frame bytes.Buffer
	frame.Write(first)
	rem := 0
	for shift := uint(0); ; shift += 7 {
		b := make([]byte, 1)
		if _, err := io.ReadFull(conn, b); err != nil {
			t.Fatalf("reading remaining length: %v", err)
		}
		frame.Write(b)
		rem |= int(b[0]&0x7F) << shift
		if b[0]&0x80 == 0 {
			break
		}
		if shift > 21 {
			t.Fatal("remaining length too long")
		}
	}
	payload := make([]byte, rem)
	if _, err := io.ReadFull(conn, payload); err != nil {
		t.Fatalf("reading packet payload: %v", err)
	}
	frame.Write(payload)
	return frame.Bytes()
}

func writeFrame(t *testing.T, conn net.Conn, b []byte) {
	t.Helper()
	_ = conn.SetWriteDeadline(time.Now().Add(testTimeout))
	if _, err := conn.Write(b); err != nil {
		t.Fatalf("writing % x: %v", b, err)
	}
}

func expectFrame(t *testing.T, conn net.Conn, want []byte, name string) {
	t.Helper()
	got := readFrame(t, conn)
	if !bytes.Equal(got, want) {
		t.Fatalf("%s: expected packet % x, got % x", name, want, got)
	}
}

func connAckFrame(sessionPresent bool, code ConnectionReturnCode) []byte {
	sp := byte(0)
	if sessionPresent {
		sp = 1
	}
	return []byte{0x20, 0x02, sp, byte(code)}
}

func publishFrame(id uint16, topic, payload string, qos QoS, dup bool) []byte {
	return (&pktPublish{Message: &Message{
		Topic:   topic,
		ID:      id,
		QoS:     qos,
		Payload: []byte(payload),
		Dup:     dup,
	}}).Pack()
}

func pubRecFrame(id uint16) []byte  { return (&pktPubRec{ID: id}).Pack() }
func pubRelFrame(id uint16) []byte  { return (&pktPubRel{ID: id}).Pack() }
func pubCompFrame(id uint16) []byte { return (&pktPubComp{ID: id}).Pack() }
func pubAckFrame(id uint16) []byte  { return (&pktPubAck{ID: id}).Pack() }

type connectResult struct {
	sessionPresent bool
	err            error
}

func connectAsync(t *testing.T, cli *BaseClient, clean bool) <-chan connectResult {
	t.Helper()
	ch := make(chan connectResult, 1)
	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), testTimeout)
		defer cancel()
		var opts []ConnectOption
		if clean {
			opts = append(opts, WithCleanSession(true))
		}
		sp, err := cli.Connect(ctx, "inbound-session-test", opts...)
		ch <- connectResult{sessionPresent: sp, err: err}
	}()
	return ch
}

func waitConnect(t *testing.T, ch <-chan connectResult) connectResult {
	t.Helper()
	select {
	case r := <-ch:
		return r
	case <-time.After(testTimeout):
		t.Fatal("timeout waiting for Connect to return")
		return connectResult{}
	}
}

func waitDone(t *testing.T, cli *BaseClient) {
	t.Helper()
	select {
	case <-cli.Done():
	case <-time.After(testTimeout):
		t.Fatal("timeout waiting for client connection to close")
	}
}

// newClientWithPending drives a fresh connection up to PUBREC and then drops
// it, leaving one QoS 2 message pending in the returned client's session.
func newClientWithPending(t *testing.T, id uint16, topic, payload string) *BaseClient {
	t.Helper()
	ca, cb := net.Pipe()
	cli := &BaseClient{Transport: cb}
	cli.Handle(&recHandler{})

	res := connectAsync(t, cli, false)
	expectFrame(t, ca, (&pktConnect{
		ProtocolLevel: ProtocolLevel4,
		ClientID:      "inbound-session-test",
	}).Pack(), "CONNECT")
	writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
	if r := waitConnect(t, res); r.err != nil {
		t.Fatalf("first Connect failed: %v", r.err)
	}

	writeFrame(t, ca, publishFrame(id, topic, payload, QoS2, false))
	expectFrame(t, ca, pubRecFrame(id), "PUBREC")
	if n := cli.InboundSession().Pending(); n != 1 {
		t.Fatalf("expected 1 pending message, got %d", n)
	}

	// Drop the transport after PUBREC, before any PUBREL.
	if err := ca.Close(); err != nil {
		t.Fatalf("closing broker end: %v", err)
	}
	waitDone(t, cli)
	// Ending the old serving loop must not clear handed-over state.
	if n := cli.InboundSession().Pending(); n != 1 {
		t.Fatalf("pending state lost on disconnect: expected 1, got %d", n)
	}
	return cli
}

func TestInboundSession(t *testing.T) {
	t.Run("SessionStateCloneIsolation", func(t *testing.T) {
		s := &InboundSession{}
		original := &Message{Topic: "t", ID: 1, QoS: QoS2, Payload: []byte("abc")}
		if err := s.store(original); err != nil {
			t.Fatalf("unexpected error: %v", err)
		}
		// Mutating the parsed packet must not change the protocol record.
		original.Topic = "u"
		original.Payload[0] = 'X'
		stored := s.take(1)
		if stored == nil {
			t.Fatal("stored message disappeared")
		}
		if stored.Topic != "t" || !bytes.Equal(stored.Payload, []byte("abc")) {
			t.Fatalf("protocol record was mutated: topic=%q payload=%q", stored.Topic, stored.Payload)
		}
		if s.take(1) != nil {
			t.Fatal("second take must not return the message again")
		}
		if s.take(4242) != nil {
			t.Fatal("unknown identifier must not return a message")
		}
		if s.Pending() != 0 {
			t.Fatalf("expected empty session, got %d pending", s.Pending())
		}
	})

	t.Run("ResumeAfterPubRec", func(t *testing.T) {
		const id uint16 = 17
		cli1 := newClientWithPending(t, id, "a", "234")
		h := &recHandler{}

		// A brand new BaseClient explicitly takes over the MQTT session.
		ca, cb := net.Pipe()
		cli2 := &BaseClient{Transport: cb}
		cli2.Handle(h)
		cli2.TransferInboundSession(cli1)
		if cli2.InboundSession() != cli1.InboundSession() {
			t.Fatal("handed-over session must be the same instance")
		}

		res := connectAsync(t, cli2, false)
		expectFrame(t, ca, (&pktConnect{
			ProtocolLevel: ProtocolLevel4,
			ClientID:      "inbound-session-test",
		}).Pack(), "CONNECT")
		writeFrame(t, ca, connAckFrame(true, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil || !r.sessionPresent {
			t.Fatalf("second Connect: sessionPresent=%v err=%v", r.sessionPresent, r.err)
		}

		// The broker resumes the QoS 2 handshake with PUBREL only.
		writeFrame(t, ca, pubRelFrame(id))
		expectFrame(t, ca, []byte{0x70, 0x02, 0x00, 0x11}, "PUBCOMP")

		if h.calls() != 1 {
			t.Fatalf("expected exactly 1 callback, got %d", h.calls())
		}
		payloads := h.payloads()
		if !bytes.Equal(payloads[0], []byte("234")) {
			t.Fatalf("expected payload '234', got %q", payloads[0])
		}
		if cli2.InboundSession().Pending() != 0 {
			t.Fatalf("session must be empty after PUBCOMP, got %d", cli2.InboundSession().Pending())
		}
	})

	t.Run("DuplicatePublishAndPubRel", func(t *testing.T) {
		const id uint16 = 20
		ca, cb := net.Pipe()
		cli := &BaseClient{Transport: cb}
		h := &recHandler{}
		cli.Handle(h)

		res := connectAsync(t, cli, false)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}

		// First PUBLISH: stored and answered with PUBREC, not delivered.
		writeFrame(t, ca, publishFrame(id, "t", "dup", QoS2, false))
		expectFrame(t, ca, []byte{0x50, 0x02, 0x00, 0x14}, "first PUBREC")
		// Retransmitted PUBLISH with identical content: another PUBREC,
		// but the stored message is not replaced and nothing is delivered.
		writeFrame(t, ca, publishFrame(id, "t", "dup", QoS2, true))
		expectFrame(t, ca, []byte{0x50, 0x02, 0x00, 0x14}, "retransmitted PUBREC")
		if h.calls() != 0 || cli.InboundSession().Pending() != 1 {
			t.Fatalf("calls=%d pending=%d, want 0 calls and 1 pending", h.calls(), cli.InboundSession().Pending())
		}

		// First PUBREL delivers once; the callback is allowed to mutate.
		writeFrame(t, ca, pubRelFrame(id))
		expectFrame(t, ca, pubCompFrame(id), "first PUBCOMP")
		if h.calls() != 1 {
			t.Fatalf("expected 1 callback after PUBREL, got %d", h.calls())
		}
		h.mu.Lock()
		h.msgs[0].Payload[0] = 'X'
		h.mu.Unlock()

		// Duplicate PUBREL: still answered, never delivered twice.
		writeFrame(t, ca, pubRelFrame(id))
		expectFrame(t, ca, pubCompFrame(id), "duplicate PUBCOMP")
		// Unknown PUBREL: answered as well.
		writeFrame(t, ca, pubRelFrame(200))
		expectFrame(t, ca, pubCompFrame(200), "unknown PUBCOMP")
		if h.calls() != 1 {
			t.Fatalf("duplicate PUBREL must not redeliver, got %d calls", h.calls())
		}
	})

	t.Run("ConflictingRetransmission", func(t *testing.T) {
		const id uint16 = 21
		ca, cb := net.Pipe()
		cli := &BaseClient{Transport: cb}
		cli.Handle(&recHandler{})

		res := connectAsync(t, cli, false)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}

		writeFrame(t, ca, publishFrame(id, "t", "one", QoS2, false))
		expectFrame(t, ca, pubRecFrame(id), "PUBREC")

		// Same identifier, contradicting payload: must error, not overwrite.
		writeFrame(t, ca, publishFrame(id, "t", "two", QoS2, true))
		waitDone(t, cli)
		if err := cli.Err(); !errors.Is(err, ErrInboundSessionConflict) {
			t.Fatalf("expected ErrInboundSessionConflict, got %v", err)
		}

		// The original reading survives untouched.
		if cli.InboundSession().Pending() != 1 {
			t.Fatalf("conflicting packet altered session: pending=%d", cli.InboundSession().Pending())
		}
		stored := cli.InboundSession().take(id)
		if stored == nil || !bytes.Equal(stored.Payload, []byte("one")) {
			t.Fatalf("original message was overwritten: %v", stored)
		}
	})

	t.Run("ConflictingRetransmissionTopic", func(t *testing.T) {
		const id uint16 = 22
		ca, cb := net.Pipe()
		cli := &BaseClient{Transport: cb}
		cli.Handle(&recHandler{})

		res := connectAsync(t, cli, false)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}

		writeFrame(t, ca, publishFrame(id, "topic-a", "same", QoS2, false))
		expectFrame(t, ca, pubRecFrame(id), "PUBREC")

		// Same identifier and payload but a different topic.
		writeFrame(t, ca, publishFrame(id, "topic-b", "same", QoS2, true))
		waitDone(t, cli)
		if err := cli.Err(); !errors.Is(err, ErrInboundSessionConflict) {
			t.Fatalf("expected ErrInboundSessionConflict, got %v", err)
		}
		stored := cli.InboundSession().take(id)
		if stored == nil || stored.Topic != "topic-a" {
			t.Fatalf("original topic was overwritten: %v", stored)
		}
	})

	t.Run("ReuseIDAfterCompletion", func(t *testing.T) {
		const id uint16 = 30
		ca, cb := net.Pipe()
		cli := &BaseClient{Transport: cb}
		h := &recHandler{}
		cli.Handle(h)

		res := connectAsync(t, cli, false)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}

		for _, payload := range []string{"first", "second"} {
			writeFrame(t, ca, publishFrame(id, "t", payload, QoS2, false))
			expectFrame(t, ca, pubRecFrame(id), "PUBREC")
			writeFrame(t, ca, pubRelFrame(id))
			expectFrame(t, ca, pubCompFrame(id), "PUBCOMP")
		}

		if h.calls() != 2 {
			t.Fatalf("expected 2 deliveries for reused ID, got %d", h.calls())
		}
		payloads := h.payloads()
		if !bytes.Equal(payloads[0], []byte("first")) || !bytes.Equal(payloads[1], []byte("second")) {
			t.Fatalf("unexpected payload sequence: %q, %q", payloads[0], payloads[1])
		}
	})

	t.Run("TwoClientsSameID", func(t *testing.T) {
		const id uint16 = 40
		type pair struct {
			cli *BaseClient
			ca  net.Conn
			h   *recHandler
		}
		var pairs [2]pair
		for i := range pairs {
			ca, cb := net.Pipe()
			cli := &BaseClient{Transport: cb}
			h := &recHandler{}
			cli.Handle(h)
			pairs[i] = pair{cli: cli, ca: ca, h: h}

			res := connectAsync(t, cli, true)
			readFrame(t, ca) // CONNECT
			writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
			if r := waitConnect(t, res); r.err != nil {
				t.Fatalf("Connect %d failed: %v", i, r.err)
			}
		}
		if pairs[0].cli.InboundSession() == pairs[1].cli.InboundSession() {
			t.Fatal("independent clients must not share an inbound session")
		}

		for i, payload := range []string{"client-a", "client-b"} {
			p := pairs[i]
			writeFrame(t, p.ca, publishFrame(id, "t", payload, QoS2, false))
			expectFrame(t, p.ca, pubRecFrame(id), "PUBREC")
			writeFrame(t, p.ca, pubRelFrame(id))
			expectFrame(t, p.ca, pubCompFrame(id), "PUBCOMP")
		}
		for i, payload := range []string{"client-a", "client-b"} {
			p := pairs[i]
			if p.h.calls() != 1 {
				t.Fatalf("client %d: expected 1 callback, got %d", i, p.h.calls())
			}
			got := p.h.payloads()[0]
			if !bytes.Equal(got, []byte(payload)) {
				t.Fatalf("client %d: expected payload %q, got %q", i, payload, got)
			}
		}
	})

	t.Run("SameIDInboundAndOutbound", func(t *testing.T) {
		const id uint16 = 50
		ca, cb := net.Pipe()
		cli := &BaseClient{Transport: cb}
		h := &recHandler{}
		cli.Handle(h)

		res := connectAsync(t, cli, false)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}

		// Outgoing QoS 2 publish using the same packet identifier as an
		// incoming one. The signaller channels and inbound session must not
		// cross.
		pubErr := make(chan error, 1)
		go func() {
			ctx, cancel := context.WithTimeout(context.Background(), testTimeout)
			defer cancel()
			pubErr <- cli.Publish(ctx, &Message{
				Topic: "out", ID: id, QoS: QoS2, Payload: []byte("out-payload"),
			})
		}()
		expectFrame(t, ca, publishFrame(id, "out", "out-payload", QoS2, false), "outgoing PUBLISH")

		// Inbound handshake.
		writeFrame(t, ca, publishFrame(id, "in", "in-payload", QoS2, false))
		expectFrame(t, ca, pubRecFrame(id), "inbound PUBREC")

		// Broker acknowledges the outgoing PUBLISH; client answers PUBREL.
		writeFrame(t, ca, pubRecFrame(id))
		expectFrame(t, ca, pubRelFrame(id), "outgoing PUBREL")

		// Broker releases the inbound one; client answers PUBCOMP.
		writeFrame(t, ca, pubRelFrame(id))
		expectFrame(t, ca, pubCompFrame(id), "inbound PUBCOMP")

		// Broker completes the outgoing handshake.
		writeFrame(t, ca, pubCompFrame(id))
		select {
		case err := <-pubErr:
			if err != nil {
				t.Fatalf("outgoing Publish failed: %v", err)
			}
		case <-time.After(testTimeout):
			t.Fatal("timeout waiting for outgoing Publish to complete")
		}

		if h.calls() != 1 {
			t.Fatalf("expected 1 inbound callback, got %d", h.calls())
		}
		got := h.payloads()[0]
		if !bytes.Equal(got, []byte("in-payload")) {
			t.Fatalf("inbound payload crossed with outgoing one: %q", got)
		}
	})

	t.Run("CleanSessionDropsPending", func(t *testing.T) {
		const id uint16 = 60
		cli1 := newClientWithPending(t, id, "t", "dropped-clean")
		h := &recHandler{}

		ca, cb := net.Pipe()
		cli2 := &BaseClient{Transport: cb}
		cli2.Handle(h)
		cli2.TransferInboundSession(cli1)

		res := connectAsync(t, cli2, true)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(true, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}
		if cli2.InboundSession().Pending() != 0 {
			t.Fatalf("CleanSession must drop pending state, got %d", cli2.InboundSession().Pending())
		}

		writeFrame(t, ca, pubRelFrame(id))
		expectFrame(t, ca, pubCompFrame(id), "PUBCOMP")
		if h.calls() != 0 {
			t.Fatalf("clean session must not deliver stale reading, got %d calls", h.calls())
		}
	})

	t.Run("SessionPresentFalseDropsPending", func(t *testing.T) {
		const id uint16 = 61
		cli1 := newClientWithPending(t, id, "t", "dropped-no-session")
		h := &recHandler{}

		ca, cb := net.Pipe()
		cli2 := &BaseClient{Transport: cb}
		cli2.Handle(h)
		cli2.TransferInboundSession(cli1)

		res := connectAsync(t, cli2, false)
		readFrame(t, ca) // CONNECT
		// CONNACK and the stale PUBREL arrive in the same write. The reset
		// must happen before the PUBREL is processed.
		writeFrame(t, ca, append(connAckFrame(false, ConnectionAccepted), pubRelFrame(id)...))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}
		expectFrame(t, ca, pubCompFrame(id), "PUBCOMP")
		if h.calls() != 0 {
			t.Fatalf("SessionPresent=false must not deliver stale reading, got %d calls", h.calls())
		}
		if cli2.InboundSession().Pending() != 0 {
			t.Fatalf("expected empty session, got %d pending", cli2.InboundSession().Pending())
		}
	})

	t.Run("ConnAckAndPubRelSameRead", func(t *testing.T) {
		const id uint16 = 70
		cli1 := newClientWithPending(t, id, "t", "same-read")
		h := &recHandler{}

		ca, cb := net.Pipe()
		cli2 := &BaseClient{Transport: cb}
		cli2.Handle(h)
		cli2.TransferInboundSession(cli1)

		res := connectAsync(t, cli2, false)
		readFrame(t, ca) // CONNECT
		// The broker pipelines CONNACK and the pending PUBREL.
		writeFrame(t, ca, append(connAckFrame(true, ConnectionAccepted), pubRelFrame(id)...))
		if r := waitConnect(t, res); r.err != nil || !r.sessionPresent {
			t.Fatalf("Connect: sessionPresent=%v err=%v", r.sessionPresent, r.err)
		}
		expectFrame(t, ca, pubCompFrame(id), "PUBCOMP")

		if h.calls() != 1 {
			t.Fatalf("expected the pipelined PUBREL to deliver once, got %d calls", h.calls())
		}
		if got := h.payloads()[0]; !bytes.Equal(got, []byte("same-read")) {
			t.Fatalf("unexpected payload: %q", got)
		}
	})

	t.Run("RejectedConnAckKeepsState", func(t *testing.T) {
		const id uint16 = 90
		cli1 := newClientWithPending(t, id, "t", "kept-on-refusal")

		ca, cb := net.Pipe()
		cli2 := &BaseClient{Transport: cb}
		cli2.TransferInboundSession(cli1)

		res := connectAsync(t, cli2, false)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(true, NotAuthorized))
		r := waitConnect(t, res)
		if r.err == nil {
			t.Fatal("rejected CONNACK must return an error")
		}
		var connErr *ConnectionError
		if !errors.As(r.err, &connErr) || connErr.Code != NotAuthorized {
			t.Fatalf("expected NotAuthorized ConnectionError, got %v", r.err)
		}
		if cli2.InboundSession().Pending() != 1 {
			t.Fatalf("rejected connection must not clear state, pending=%d", cli2.InboundSession().Pending())
		}
		stored := cli2.InboundSession().take(id)
		if stored == nil || !bytes.Equal(stored.Payload, []byte("kept-on-refusal")) {
			t.Fatalf("pending reading was lost: %v", stored)
		}
	})

	t.Run("ConnectFailureKeepsState", func(t *testing.T) {
		const id uint16 = 91
		cli1 := newClientWithPending(t, id, "t", "kept-on-failure")

		ca, cb := net.Pipe()
		cli2 := &BaseClient{Transport: cb}
		cli2.TransferInboundSession(cli1)

		res := connectAsync(t, cli2, false)
		readFrame(t, ca) // CONNECT
		// Broker vanishes before any CONNACK: establishment fails.
		if err := ca.Close(); err != nil {
			t.Fatalf("closing broker end: %v", err)
		}
		r := waitConnect(t, res)
		if r.err == nil {
			t.Fatal("failed establishment must return an error")
		}
		waitDone(t, cli2)
		if cli2.InboundSession().Pending() != 1 {
			t.Fatalf("failed establishment must not clear state, pending=%d",
				cli2.InboundSession().Pending())
		}
		stored := cli2.InboundSession().take(id)
		if stored == nil || !bytes.Equal(stored.Payload, []byte("kept-on-failure")) {
			t.Fatalf("pending reading was lost: %v", stored)
		}
	})

	t.Run("QoS1AllowsDuplicateDelivery", func(t *testing.T) {
		const id uint16 = 100
		ca, cb := net.Pipe()
		cli := &BaseClient{Transport: cb}
		h := &recHandler{}
		cli.Handle(h)

		res := connectAsync(t, cli, false)
		readFrame(t, ca) // CONNECT
		writeFrame(t, ca, connAckFrame(false, ConnectionAccepted))
		if r := waitConnect(t, res); r.err != nil {
			t.Fatalf("Connect failed: %v", r.err)
		}

		for i := 0; i < 2; i++ {
			writeFrame(t, ca, publishFrame(id, "t", "at-least-once", QoS1, i == 1))
			expectFrame(t, ca, pubAckFrame(id), "PUBACK")
		}
		if h.calls() != 2 {
			t.Fatalf("QoS 1 duplicates must both be delivered, got %d calls", h.calls())
		}
	})

	t.Run("ReconnectContinuesSession", func(t *testing.T) {
		const id uint16 = 80
		conns := make(chan net.Conn, 4)
		var mu sync.Mutex
		var dials []*BaseClient
		dialer := DialerFunc(func(ctx context.Context) (*BaseClient, error) {
			ca, cb := net.Pipe()
			conns <- ca
			cli := &BaseClient{Transport: cb}
			mu.Lock()
			dials = append(dials, cli)
			mu.Unlock()
			return cli, nil
		})

		rc, err := NewReconnectClient(dialer,
			WithReconnectWait(time.Millisecond, 5*time.Millisecond),
		)
		if err != nil {
			t.Fatalf("NewReconnectClient: %v", err)
		}
		h := &recHandler{notify: make(chan struct{}, 8)}
		rc.Handle(h)

		ctx, cancel := context.WithTimeout(context.Background(), testTimeout)
		defer cancel()
		connectErr := make(chan error, 1)
		go func() {
			_, err := rc.Connect(ctx, "reconnect-client", WithCleanSession(false))
			connectErr <- err
		}()

		// First connection: start the QoS 2 handshake, then drop.
		var ca1 net.Conn
		select {
		case ca1 = <-conns:
		case <-time.After(testTimeout):
			t.Fatal("timeout waiting for first dial")
		}
		readFrame(t, ca1) // CONNECT
		writeFrame(t, ca1, connAckFrame(false, ConnectionAccepted))
		select {
		case err := <-connectErr:
			if err != nil {
				t.Fatalf("first Connect failed: %v", err)
			}
		case <-time.After(testTimeout):
			t.Fatal("timeout waiting for reconnect Connect")
		}

		retry := rc.(*reconnectClient).RetryClient
		firstSession := retry.Client().InboundSession()
		if firstSession == nil {
			t.Fatal("missing inbound session on first client")
		}

		writeFrame(t, ca1, publishFrame(id, "t", "reconnect-payload", QoS2, false))
		expectFrame(t, ca1, pubRecFrame(id), "PUBREC")
		if err := ca1.Close(); err != nil {
			t.Fatalf("closing first connection: %v", err)
		}

		// Reconnected transport takes over the handshake.
		var ca2 net.Conn
		select {
		case ca2 = <-conns:
		case <-time.After(testTimeout):
			t.Fatal("timeout waiting for reconnect dial")
		}
		readFrame(t, ca2) // CONNECT
		writeFrame(t, ca2, connAckFrame(true, ConnectionAccepted))
		writeFrame(t, ca2, pubRelFrame(id))
		expectFrame(t, ca2, pubCompFrame(id), "PUBCOMP")

		h.waitCall(t, "delivery across reconnect")
		if h.calls() != 1 {
			t.Fatalf("expected 1 callback across reconnect, got %d", h.calls())
		}
		if got := h.payloads()[0]; !bytes.Equal(got, []byte("reconnect-payload")) {
			t.Fatalf("unexpected payload after reconnect: %q", got)
		}

		if retry.Client() == dials[0] {
			t.Fatal("underlying BaseClient was not replaced on reconnect")
		}
		if got := retry.Client().InboundSession(); got != firstSession {
			t.Fatalf("inbound session was not propagated to the new BaseClient: %p != %p", got, firstSession)
		}
		if retry.Stats().CountSetClient < 2 {
			t.Fatalf("expected at least 2 SetClient calls, got %d", retry.Stats().CountSetClient)
		}

		disconnectErr := make(chan error, 1)
		go func() { disconnectErr <- rc.Disconnect(ctx) }()
		expectFrame(t, ca2, []byte{0xE0, 0x00}, "DISCONNECT")
		select {
		case err := <-disconnectErr:
			if err != nil {
				t.Fatalf("Disconnect failed: %v", err)
			}
		case <-time.After(testTimeout):
			t.Fatal("timeout waiting for Disconnect")
		}
	})
}
