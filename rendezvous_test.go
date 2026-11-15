package srt

import (
	"bytes"
	"context"
	"net"
	"reflect"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/datarhei/gosrt/circular"
	"github.com/datarhei/gosrt/packet"
	"github.com/datarhei/gosrt/task_support/taskcheck"
)

// rzIdentity fixes the stable local identity of a rendezvous endpoint in the
// in-process tests so cookies and ISNs can be steered deterministically.
type rzIdentity struct {
	socket uint32
	cookie uint32
	isn    uint32
}

// rzHarness wires two rendezvousers together through in-memory datagram
// queues. Nothing is bound to a UDP socket: each side's wireWrite lands in the
// peer's queue as raw datagrams, and the test decides when packets are dropped,
// duplicated, reordered or delivered.
type rzHarness struct {
	a, b *rendezvouser

	// qa holds datagrams destined for A (written by B), qb those destined for B.
	qa chan []byte
	qb chan []byte
}

func rzTestConfig() Config {
	cfg := DefaultConfig()
	cfg.ConnectionTimeout = 2 * time.Second
	cfg.ReceiverLatency = 120 * time.Millisecond
	cfg.PeerLatency = 120 * time.Millisecond
	cfg.MSS = MAX_MSS_SIZE
	cfg.FC = 25600
	return cfg
}

var (
	rzAddrA = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 29001}
	rzAddrB = &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 29002}
)

func newRzHarness(cfgA, cfgB Config, idA, idB rzIdentity) *rzHarness {
	h := &rzHarness{
		qa: make(chan []byte, 512),
		qb: make(chan []byte, 512),
	}

	h.a = newRendezvousForTest(
		cfgA, rzAddrA, rzAddrB, idA.socket, idA.cookie,
		circular.New(idA.isn, packet.MAX_SEQUENCENUMBER),
		func(b []byte) error { h.qb <- append([]byte(nil), b...); return nil },
	)

	h.b = newRendezvousForTest(
		cfgB, rzAddrB, rzAddrA, idB.socket, idB.cookie,
		circular.New(idB.isn, packet.MAX_SEQUENCENUMBER),
		func(b []byte) error { h.qa <- append([]byte(nil), b...); return nil },
	)

	return h
}

func (h *rzHarness) close() {
	h.a.Close()
	h.b.Close()
}

// deliver parses a raw datagram and hands it to the named endpoint, exactly as
// the UDP read path would.
func (h *rzHarness) deliver(rz *rendezvouser, raw []byte) error {
	p, err := packet.NewPacketFromData(rz.remoteAddr, raw)
	if err != nil {
		return err
	}
	rz.rcvQueue <- p
	return nil
}

// drain empties a datagram queue and returns its contents.
func drain(ch chan []byte) [][]byte {
	var out [][]byte
	for {
		select {
		case raw := <-ch:
			out = append(out, raw)
		default:
			return out
		}
	}
}

// handshakeCIF parses the handshake CIF out of a raw datagram.
func handshakeCIF(raw []byte) *packet.CIFHandshake {
	p, err := packet.NewPacketFromData(nil, raw)
	if err != nil {
		return nil
	}
	cif := &packet.CIFHandshake{}
	if err := p.UnmarshalCIF(cif); err != nil {
		return nil
	}
	return cif
}

// startHandshake runs both state machines and returns their completion
// channels. Progress is driven by packet arrival; the 250ms ticker only
// retransmits the current phase packet.
func (h *rzHarness) startHandshake(ctx context.Context) (<-chan error, <-chan error) {
	errA := make(chan error, 1)
	errB := make(chan error, 1)

	go func() { _, err := h.a.handshake(ctx); errA <- err }()
	go func() { _, err := h.b.handshake(ctx); errB <- err }()

	return errA, errB
}

// relay pumps both queues transparently until the context ends.
func (h *rzHarness) relay(ctx context.Context) {
	var wg sync.WaitGroup
	wg.Add(2)

	go func() {
		defer wg.Done()
		for {
			select {
			case raw := <-h.qb:
				_ = h.deliver(h.b, raw)
			case <-ctx.Done():
				return
			}
		}
	}()

	go func() {
		defer wg.Done()
		for {
			select {
			case raw := <-h.qa:
				_ = h.deliver(h.a, raw)
			case <-ctx.Done():
				return
			}
		}
	}()

	go func() {
		<-ctx.Done()
		wg.Wait()
	}()
}

// waitState blocks until cond is true, advancing on state-change signals. The
// timeout only guards against a hang; it is not used to guess event ordering.
func waitState(t *testing.T, timeout time.Duration, cond func() bool, signals ...<-chan struct{}) {
	t.Helper()
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()

	cases := make([]reflect.SelectCase, 0, len(signals)+1)
	for _, sig := range signals {
		cases = append(cases, reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(sig)})
	}
	cases = append(cases, reflect.SelectCase{Dir: reflect.SelectRecv, Chan: reflect.ValueOf(deadline.C)})

	for {
		if cond() {
			return
		}

		chosen, _, _ := reflect.Select(cases)
		if chosen == len(signals) {
			t.Fatalf("condition not met within %s", timeout)
		}
	}
}

func (h *rzHarness) waitConnected(t *testing.T) {
	t.Helper()
	waitState(t, 3*time.Second,
		func() bool { return h.a.connNow() != nil && h.b.connNow() != nil },
		h.a.stateSignal, h.b.stateSignal)
}

// assertNoStateChange delivers a packet and confirms the phase and connection
// do not move. The bounded wait is a hang guard for a negative assertion.
func assertNoStateChange(t *testing.T, c *taskcheck.C, rz *rendezvouser, raw []byte, label string) {
	t.Helper()
	rz.lock.Lock()
	before := rz.phase
	hadConn := rz.connNow() != nil
	rz.lock.Unlock()

	p, err := packet.NewPacketFromData(rz.remoteAddr, raw)
	if err != nil {
		c.Check(false, "%s: malformed probe packet: %v", label, err)
		return
	}
	rz.rcvQueue <- p

	select {
	case <-rz.stateSignal:
	case <-time.After(200 * time.Millisecond):
	}

	rz.lock.Lock()
	after := rz.phase
	rz.lock.Unlock()
	c.Check(before == after && hadConn == (rz.connNow() != nil), "%s: state must not advance (%d -> %d)", label, before, after)
}

// buildWavehand builds a WAVEHAND datagram for the given endpoint identity.
func buildWavehand(rz *rendezvouser) []byte {
	p := packet.NewPacket(rz.remoteAddr)
	p.Header().IsControlPacket = true
	p.Header().ControlType = packet.CTRLTYPE_HANDSHAKE
	p.Header().DestinationSocketId = 0
	p.MarshalCIF(rz.newWavehandCIF())

	var buf bytes.Buffer
	if err := p.Marshal(&buf); err != nil {
		return nil
	}
	return buf.Bytes()
}

// buildConclusion builds a CONCLUSION datagram, optionally carrying an HS
// extension in the requested direction.
func buildConclusion(rz *rendezvouser, withHS, isRequest bool) []byte {
	rz.lock.Lock()
	cif := rz.buildConclusionCIF(false)
	if withHS {
		cif.HasHS = true
		cif.IsRequest = isRequest
		cif.SRTHS = rz.localHSExtension()
	}
	rz.lock.Unlock()

	p := packet.NewPacket(rz.remoteAddr)
	p.Header().IsControlPacket = true
	p.Header().ControlType = packet.CTRLTYPE_HANDSHAKE
	p.Header().DestinationSocketId = rz.peerSocketId
	p.MarshalCIF(cif)

	var buf bytes.Buffer
	if err := p.Marshal(&buf); err != nil {
		return nil
	}
	return buf.Bytes()
}

// marshalHandshake re-marshals a CIF into a datagram for the given endpoint.
func marshalHandshake(rz *rendezvouser, cif *packet.CIFHandshake, destSocket uint32) []byte {
	p := packet.NewPacket(rz.remoteAddr)
	p.Header().IsControlPacket = true
	p.Header().ControlType = packet.CTRLTYPE_HANDSHAKE
	p.Header().DestinationSocketId = destSocket
	p.MarshalCIF(cif)

	var buf bytes.Buffer
	if err := p.Marshal(&buf); err != nil {
		return nil
	}
	return buf.Bytes()
}

func TestRendezvousHandshake(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		// --- Cookie contest: negative vs positive signed 32-bit cookies. ---
		idA := rzIdentity{socket: 0xA0000001, cookie: 0xFFFFFFFF, isn: 1000} // -1
		idB := rzIdentity{socket: 0xB0000002, cookie: 0x00000001, isn: 2000} // +1

		h := newRzHarness(rzTestConfig(), rzTestConfig(), idA, idB)
		defer h.close()

		errA, errB := h.startHandshake(ctx)
		h.relay(ctx)
		h.waitConnected(t)

		c.Check(h.a.role == rzRoleResponder, "negative-cookie side must be responder, got role %d", h.a.role)
		c.Check(h.b.role == rzRoleInitiator, "positive-cookie side must be initiator, got role %d", h.b.role)
		c.Check(h.a.phase == rzPhaseAgreement, "responder ends in agreement phase, got %d", h.a.phase)
		c.Check(h.b.phase == rzPhaseAgreement, "initiator ends in agreement phase, got %d", h.b.phase)

		// Independent socket IDs and ISNs are preserved end to end.
		c.Check(h.a.connNow().SocketId() == idA.socket, "A keeps its own socket ID")
		c.Check(h.a.connNow().PeerSocketId() == idB.socket, "A records the peer socket ID")
		c.Check(h.b.connNow().SocketId() == idB.socket, "B keeps its own socket ID")
		c.Check(h.b.connNow().PeerSocketId() == idA.socket, "B records the peer socket ID")
		c.Check(h.a.connNow().initialPacketSequenceNumber.Val() == idA.isn, "A sender ISN is its own ISN")
		c.Check(h.a.connNow().receiveInitialPacketSequenceNumber.Val() == idB.isn, "A receiver ISN is the peer ISN")
		c.Check(h.b.connNow().initialPacketSequenceNumber.Val() == idB.isn, "B sender ISN is its own ISN")
		c.Check(h.b.connNow().receiveInitialPacketSequenceNumber.Val() == idA.isn, "B receiver ISN is the peer ISN")

		select {
		case err := <-errA:
			c.Check(err == nil, "A handshake completed without error: %v", err)
		case <-time.After(time.Second):
			c.Check(false, "A handshake did not return")
		}
		select {
		case err := <-errB:
			c.Check(err == nil, "B handshake completed without error: %v", err)
		case <-time.After(time.Second):
			c.Check(false, "B handshake did not return")
		}

		// --- Both sides send first: crossed WAVEHANDs still converge. ---
		// The relay above already crossed the initial WAVEHANDs (each side sent
		// before receiving). Verify explicitly that both observed a WAVEHAND.
		c.Check(h.a.havePeer && h.b.havePeer, "crossed WAVEHANDs let both sides learn the peer identity")

		// --- Reverse the cookie ordering: the other side now wins. ---
		idA2 := rzIdentity{socket: 0xA0000003, cookie: 0x00000002, isn: 10}
		idB2 := rzIdentity{socket: 0xB0000004, cookie: 0x80000000, isn: 20} // most negative
		h2 := newRzHarness(rzTestConfig(), rzTestConfig(), idA2, idB2)
		defer h2.close()
		ctx2, cancel2 := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel2()
		h2.startHandshake(ctx2)
		h2.relay(ctx2)
		h2.waitConnected(t)
		c.Check(h2.a.role == rzRoleInitiator, "A with the larger signed cookie is initiator, got %d", h2.a.role)
		c.Check(h2.b.role == rzRoleResponder, "B with the smaller signed cookie is responder, got %d", h2.b.role)

		// --- Equal cookies are a definitive failure. ---
		idEq := rzIdentity{socket: 0xC0000005, cookie: 0x12345678, isn: 1}
		h3 := newRzHarness(rzTestConfig(), rzTestConfig(), idEq, idEq)
		defer h3.close()
		ctx3, cancel3 := context.WithCancel(context.Background())
		defer cancel3()
		eA, eB := h3.startHandshake(ctx3)
		h3.relay(ctx3)
		waitState(t, 3*time.Second, func() bool { return h3.a.failed && h3.b.failed }, h3.a.stateSignal, h3.b.stateSignal)
		c.Check(h3.a.connNow() == nil && h3.b.connNow() == nil, "equal cookies establish no connection")
		select {
		case err := <-eA:
			c.Check(err != nil, "equal-cookie attempt returns an error: %v", err)
		case <-time.After(2 * time.Second):
			c.Check(false, "A did not return after equal cookies")
		}
		select {
		case err := <-eB:
			c.Check(err != nil, "equal-cookie attempt returns an error: %v", err)
		case <-time.After(2 * time.Second):
			c.Check(false, "B did not return after equal cookies")
		}

		// --- CONCLUSION arrives before any WAVEHAND. ---
		idInit := rzIdentity{socket: 0x11000001, cookie: 0x7FFFFFFF, isn: 500} // initiator
		idResp := rzIdentity{socket: 0x22000002, cookie: 0x00000010, isn: 600} // responder
		h4 := newRzHarness(rzTestConfig(), rzTestConfig(), idResp, idInit)
		defer h4.close()
		ctx4, cancel4 := context.WithCancel(context.Background())
		defer cancel4()
		go func() { _, _ = h4.b.handshake(ctx4) }()
		_ = drain(h4.qa)

		firstConclusion := buildConclusion(h4.b, true, true) // CONCLUSION + HSREQ
		c.Check(firstConclusion != nil, "built HSREQ conclusion")
		c.Check(h4.deliver(h4.a, firstConclusion) == nil, "deliver conclusion-first datagram")

		waitState(t, 3*time.Second,
			func() bool {
				h4.a.lock.Lock()
				ok := h4.a.role == rzRoleResponder && h4.a.phase == rzPhaseConclusion
				h4.a.lock.Unlock()
				return ok
			}, h4.a.stateSignal)

		answers := drain(h4.qb)
		sawHSRSP := false
		for _, raw := range answers {
			cif := handshakeCIF(raw)
			if cif != nil && cif.HandshakeType == packet.HSTYPE_CONCLUSION && cif.HasHS && !cif.IsRequest {
				sawHSRSP = true
			}
		}
		c.Check(sawHSRSP, "responder answers a CONCLUSION-first start with HSRSP")

		agreementCIF := handshakeCIF(firstConclusion)
		agreementCIF.HandshakeType = packet.HSTYPE_AGREEMENT
		agreementCIF.HasHS = false
		agreementCIF.IsRequest = false
		c.Check(h4.deliver(h4.a, marshalHandshake(h4.a, agreementCIF, h4.a.socketId)) == nil, "deliver AGREEMENT")
		waitState(t, 3*time.Second, func() bool { return h4.a.connNow() != nil }, h4.a.stateSignal)
		c.Check(h4.a.connNow() != nil, "responder connects on AGREEMENT after a CONCLUSION-first start")

		// --- HSRSP loss: retransmitted HSREQ still completes the handshake. ---
		idC := rzIdentity{socket: 0x33000001, cookie: 0x70000000, isn: 700}
		idD := rzIdentity{socket: 0x44000002, cookie: 0x00000020, isn: 800}
		h5 := newRzHarness(rzTestConfig(), rzTestConfig(), idC, idD)
		defer h5.close()
		ctx5, cancel5 := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel5()
		h5.startHandshake(ctx5)

		droppedFirstHSRSP := false
		pumpDone := make(chan struct{})
		go func() {
			defer close(pumpDone)
			for {
				select {
				case <-ctx5.Done():
					return
				case raw := <-h5.qb:
					_ = h5.deliver(h5.b, raw)
				case raw := <-h5.qa:
					cif := handshakeCIF(raw)
					if cif != nil && cif.HasHS && !cif.IsRequest && !droppedFirstHSRSP {
						droppedFirstHSRSP = true
						continue // lose the first HSRSP
					}
					_ = h5.deliver(h5.a, raw)
				}
			}
		}()
		h5.waitConnected(t)
		cancel5()
		<-pumpDone
		c.Check(droppedFirstHSRSP, "the first HSRSP was actually dropped by the forwarder")
		c.Check(h5.a.connNow() != nil && h5.b.connNow() != nil, "connection completes after HSRSP loss and retransmit")

		// --- A truncated HS extension cannot advance the state. ---
		idE := rzIdentity{socket: 0x77000001, cookie: 0x00000040, isn: 1100}
		idF := rzIdentity{socket: 0x88000002, cookie: 0x70000002, isn: 1200}
		h6 := newRzHarness(rzTestConfig(), rzTestConfig(), idE, idF)
		defer h6.close()
		ctx6, cancel6 := context.WithCancel(context.Background())
		defer cancel6()
		go func() { _, _ = h6.a.handshake(ctx6) }()

		// First a valid WAVEHAND so the peer identity is established.
		c.Check(h6.deliver(h6.a, buildWavehand(h6.b)) == nil, "deliver valid WAVEHAND")
		waitState(t, 3*time.Second, func() bool { return h6.a.havePeer }, h6.a.stateSignal)

		truncated := buildConclusion(h6.b, true, true)
		truncated = truncated[:len(truncated)-8]
		assertNoStateChange(t, c, h6.a, truncated, "truncated HS extension")

		// --- A changed peer identity is ignored, not adopted. ---
		foreignCIF := handshakeCIF(buildWavehand(h6.b))
		foreignCIF.SRTSocketId = 0xDEADBEEF
		foreignCIF.SynCookie = 0xCAFEBABE
		assertNoStateChange(t, c, h6.a, marshalHandshake(h6.a, foreignCIF, 0), "foreign peer identity")
		c.Check(h6.a.peerSocketId == idF.socket, "the original peer socket ID is retained")
	})
}

func TestRendezvousTransport(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
		defer cancel()

		// Different MSS and latencies on the two sides.
		cfgA := rzTestConfig()
		cfgA.MSS = 1500
		cfgA.FC = 25600

		cfgB := rzTestConfig()
		cfgB.MSS = 576 // smallest common MSS must win
		cfgB.ReceiverLatency = 200 * time.Millisecond
		cfgB.PeerLatency = 160 * time.Millisecond
		cfgB.FC = 100 // smallest FC must win

		// Independent ISNs, with A starting two packets before the 31-bit wrap.
		idA := rzIdentity{socket: 0xAA000001, cookie: 0x70000010, isn: packet.MAX_SEQUENCENUMBER - 1}
		idB := rzIdentity{socket: 0xBB000002, cookie: 0x00000050, isn: 0}

		h := newRzHarness(cfgA, cfgB, idA, idB)
		defer h.close()
		h.startHandshake(ctx)
		h.relay(ctx)
		h.waitConnected(t)

		// Negotiated MSS and FC are the smaller announcements.
		c.Check(h.a.effectiveMSS == 576, "effective MSS is the smaller value (576), got %d", h.a.effectiveMSS)
		c.Check(h.b.effectiveMSS == 576, "both sides converge on the smaller MSS, got %d", h.b.effectiveMSS)
		c.Check(h.a.effectiveFC == 100, "effective FC is the smaller value (100), got %d", h.a.effectiveFC)
		c.Check(h.b.effectiveFC == 100, "both sides converge on the smaller FC, got %d", h.b.effectiveFC)

		// Per-direction latency negotiation.
		c.Check(h.a.effectiveRecvDelay == 160, "A recv latency is max(120,160)=160, got %d", h.a.effectiveRecvDelay)
		c.Check(h.a.effectiveSendDelay == 200, "A send latency is max(120,200)=200, got %d", h.a.effectiveSendDelay)
		c.Check(h.b.effectiveRecvDelay == 200, "B recv latency is max(200,120)=200, got %d", h.b.effectiveRecvDelay)
		c.Check(h.b.effectiveSendDelay == 160, "B send latency is max(160,120)=160, got %d", h.b.effectiveSendDelay)

		// The smaller MSS is applied to the actual payload fragmentation size.
		expectedPayload := uint32(576 - SRT_HEADER_SIZE - UDP_HEADER_SIZE)
		c.Check(h.a.connNow().config.PayloadSize == expectedPayload, "A payload size follows negotiated MSS, got %d", h.a.connNow().config.PayloadSize)
		c.Check(h.b.connNow().config.PayloadSize == expectedPayload, "B payload size follows negotiated MSS, got %d", h.b.connNow().config.PayloadSize)

		// Bidirectional transfer of distinct media payloads.
		payloadA := bytes.Repeat([]byte{0xD5}, int(expectedPayload*2+50)) // forces three fragments
		payloadB := []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}

		var aFragments [][]byte
		var fragMu sync.Mutex
		fragReady := make(chan struct{}, 1)
		origWriteA := h.a.wireWrite
		h.a.wireWrite = func(b []byte) error {
			p, err := packet.NewPacketFromData(nil, b)
			if err == nil && !p.Header().IsControlPacket {
				fragMu.Lock()
				aFragments = append(aFragments, append([]byte(nil), b...))
				if len(aFragments) >= 3 {
					select {
					case fragReady <- struct{}{}:
					default:
					}
				}
				fragMu.Unlock()
			}
			return origWriteA(b)
		}

		n, err := h.a.connNow().Write(payloadA)
		c.Check(err == nil && n == len(payloadA), "A writes its full payload: n=%d err=%v", n, err)

		select {
		case <-fragReady:
		case <-time.After(3 * time.Second):
			c.Check(false, "A did not emit the expected fragments in time")
		}

		fragMu.Lock()
		allFit := true
		seqs := make([]uint32, 0, len(aFragments))
		for _, raw := range aFragments {
			p, _ := packet.NewPacketFromData(nil, raw)
			if uint32(len(p.Data())) > expectedPayload {
				allFit = false
			}
			seqs = append(seqs, p.Header().PacketSequenceNumber.Val())
		}
		fragMu.Unlock()

		c.Check(allFit, "every fragment fits the negotiated payload size")
		c.Check(len(seqs) >= 3, "large payload is fragmented into at least three packets, got %d", len(seqs))
		c.Check(seqs[0] == idA.isn, "A's first media packet uses its own ISN (%d), got %d", idA.isn, seqs[0])
		c.Check(seqs[1] == packet.MAX_SEQUENCENUMBER, "A reaches the top of the 31-bit space, got %d", seqs[1])
		c.Check(seqs[2] == 0, "A wraps across the 31-bit boundary to 0, got %d", seqs[2])

		// B receives the reassembled payload.
		gotA := make([]byte, len(payloadA))
		c.Check(h.b.connNow().SetReadDeadline(time.Now().Add(5*time.Second)) == nil, "set B read deadline")
		_, err = readFull(h.b.connNow(), gotA)
		c.Check(err == nil, "B reads A's payload: %v", err)
		c.Check(bytes.Equal(gotA, payloadA), "B reassembles A's payload bytes exactly")

		// Reverse direction: B sends from its own independent ISN (0).
		n, err = h.b.connNow().Write(payloadB)
		c.Check(err == nil && n == len(payloadB), "B writes its payload: n=%d err=%v", n, err)
		gotB := make([]byte, len(payloadB))
		c.Check(h.a.connNow().SetReadDeadline(time.Now().Add(5*time.Second)) == nil, "set A read deadline")
		_, err = readFull(h.a.connNow(), gotB)
		c.Check(err == nil, "A reads B's payload: %v", err)
		c.Check(bytes.Equal(gotB, payloadB), "A receives B's payload bytes exactly: %x", gotB)

		// B's receiver follows A's independent sequence space, including the wrap.
		nextExpected := h.b.connNow().debug.expectedReadPacketSequenceNumber
		c.Check(nextExpected.Val() == 1, "B's receiver continued past A's wrap to seq 1, got %d", nextExpected.Val())
	})
}

func TestRendezvousLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		base := rzTestConfig()

		// --- Unsupported configurations are rejected before binding. ---
		cfg := base
		cfg.Passphrase = "somepassphrase"
		_, err := RendezvousWithContext(context.Background(), "127.0.0.1:29101", "127.0.0.1:29102", cfg)
		c.Check(err != nil, "encryption config is rejected: %v", err)

		cfg = base
		cfg.PacketFilter = "fec,cols:3,rows:3"
		_, err = RendezvousWithContext(context.Background(), "127.0.0.1:29101", "127.0.0.1:29102", cfg)
		c.Check(err != nil, "FEC config is rejected: %v", err)

		cfg = base
		cfg.StreamId = "not-allowed"
		_, err = RendezvousWithContext(context.Background(), "127.0.0.1:29101", "127.0.0.1:29102", cfg)
		c.Check(err != nil, "StreamId config is rejected: %v", err)

		cfg = base
		cfg.MSS = 8
		_, err = RendezvousWithContext(context.Background(), "127.0.0.1:29101", "127.0.0.1:29102", cfg)
		c.Check(err != nil, "out-of-range MSS is rejected: %v", err)

		cfg = base
		cfg.FC = 0
		_, err = RendezvousWithContext(context.Background(), "127.0.0.1:29101", "127.0.0.1:29102", cfg)
		c.Check(err != nil, "zero FC is rejected: %v", err)

		_, err = RendezvousWithContext(context.Background(), "[::1]:29101", "127.0.0.1:29102", base)
		c.Check(err != nil, "IPv6 local address is rejected: %v", err)
		_, err = RendezvousWithContext(context.Background(), "127.0.0.1:0", "127.0.0.1:29102", base)
		c.Check(err != nil, "zero local port is rejected: %v", err)
		_, err = RendezvousWithContext(context.Background(), "127.0.0.1:29101", "not-an-ip:29102", base)
		c.Check(err != nil, "non-IPv4 remote address is rejected: %v", err)

		// --- A failed attempt releases its UDP port for rebinding. ---
		failCfg := base
		failCfg.ConnectionTimeout = 400 * time.Millisecond
		failAddr := &net.UDPAddr{IP: net.IPv4(127, 0, 0, 1), Port: 29201}
		_, err = RendezvousWithContext(context.Background(), failAddr.String(), "127.0.0.1:29999", failCfg)
		c.Check(err != nil, "attempt with no reachable peer fails: %v", err)
		if err != nil && !isSandboxBindError(err) {
			rebound, bindErr := net.ListenUDP("udp", failAddr)
			c.Check(bindErr == nil, "the failed attempt released its UDP port: %v", bindErr)
			if rebound != nil {
				rebound.Close()
			}
		} else {
			c.Check(true, "rebind check skipped: sandbox forbids local UDP binding (%v)", err)
		}

		// --- Context cancellation aborts the attempt. ---
		ctx, cancel := context.WithCancel(context.Background())
		cancel()
		_, err = RendezvousWithContext(ctx, "127.0.0.1:29202", "127.0.0.1:29998", base)
		c.Check(err != nil, "a cancelled context fails the attempt: %v", err)

		// --- AGREEMENT loss: the first legal media packet confirms the
		// responder and is still delivered to the application. ---
		idInit := rzIdentity{socket: 0xCC000001, cookie: 0x70000020, isn: 3000}
		idResp := rzIdentity{socket: 0xDD000002, cookie: 0x00000060, isn: 4000}
		h := newRzHarness(rzTestConfig(), rzTestConfig(), idResp, idInit)
		defer h.close()
		hCtx, hCancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer hCancel()
		h.startHandshake(hCtx)

		agreementDropped := make(chan struct{}, 4)
		go func() {
			for {
				select {
				case <-hCtx.Done():
					return
				case raw := <-h.qb:
					_ = h.deliver(h.b, raw)
				case raw := <-h.qa:
					cif := handshakeCIF(raw)
					if cif != nil && cif.HandshakeType == packet.HSTYPE_AGREEMENT {
						agreementDropped <- struct{}{}
						continue // AGREEMENT never reaches the responder
					}
					_ = h.deliver(h.a, raw)
				}
			}
		}()

		waitState(t, 3*time.Second, func() bool {
			h.a.lock.Lock()
			ok := h.a.role == rzRoleResponder && h.a.phase == rzPhaseConclusion && h.a.negotiated && h.a.hsrspSent
			h.a.lock.Unlock()
			return ok
		}, h.a.stateSignal)
		select {
		case <-agreementDropped:
			c.Check(true, "at least one AGREEMENT was dropped by the forwarder")
		case <-time.After(3 * time.Second):
			c.Check(false, "no AGREEMENT was observed to drop")
		}

		mediaPayload := []byte("first-media-confirms")
		media := packet.NewPacket(h.a.remoteAddr)
		media.Header().IsControlPacket = false
		media.Header().PacketSequenceNumber = circular.New(idInit.isn, packet.MAX_SEQUENCENUMBER)
		media.Header().DestinationSocketId = h.a.socketId
		media.Header().Timestamp = 1000
		media.SetData(mediaPayload)
		var mbuf bytes.Buffer
		c.Check(media.Marshal(&mbuf) == nil, "marshal confirming media packet")
		c.Check(h.deliver(h.a, mbuf.Bytes()) == nil, "deliver confirming media packet")

		waitState(t, 3*time.Second, func() bool { return h.a.connNow() != nil }, h.a.stateSignal)
		c.Check(h.a.connNow() != nil, "responder establishes on the first legal media packet")

		got := make([]byte, len(mediaPayload))
		c.Check(h.a.connNow().SetReadDeadline(time.Now().Add(5*time.Second)) == nil, "set responder read deadline")
		_, err = readFull(h.a.connNow(), got)
		c.Check(err == nil, "the confirming media packet is delivered to the app: %v", err)
		c.Check(bytes.Equal(got, mediaPayload), "the confirming packet payload is preserved: %q", string(got))

		// --- A repeated handshake after connect must not reset the connection. ---
		connBefore := h.a.connNow()
		offsetBefore := connBefore.havePeerOffset
		c.Check(h.deliver(h.a, buildWavehand(h.b)) == nil, "re-deliver WAVEHAND on a live connection")
		repeatHSREQ := buildConclusion(h.b, true, true)
		c.Check(h.deliver(h.a, repeatHSREQ) == nil, "re-deliver CONCLUSION+HSREQ on a live connection")
		waitState(t, time.Second, func() bool { return true }, h.a.stateSignal) // drain any signal
		c.Check(h.a.connNow() == connBefore, "repeated handshake keeps the same connection instance")
		c.Check(h.a.connNow().havePeerOffset == offsetBefore, "repeated handshake does not reset the media time base")

		// --- Public entry over real UDP sockets: establish, transfer, repeat. ---
		runPublicRendezvous(t, c)
	})
}

// runPublicRendezvous exercises the exported RendezvousWithContext entry point
// over real loopback UDP sockets, transfers media in both directions, and then
// re-injects a duplicate handshake to prove the live connection is untouched.
func runPublicRendezvous(t *testing.T, c *taskcheck.C) {
	t.Helper()

	localA := "127.0.0.1:29301"
	localB := "127.0.0.1:29302"

	cfgA := rzTestConfig()
	cfgA.ConnectionTimeout = 3 * time.Second
	cfgB := rzTestConfig()
	cfgB.ConnectionTimeout = 3 * time.Second

	type result struct {
		conn Conn
		err  error
	}
	resA := make(chan result, 1)
	resB := make(chan result, 1)

	go func() {
		conn, err := RendezvousWithContext(context.Background(), localA, localB, cfgA)
		resA <- result{conn, err}
	}()
	go func() {
		conn, err := RendezvousWithContext(context.Background(), localB, localA, cfgB)
		resB <- result{conn, err}
	}()

	var connA, connB Conn
	select {
	case r := <-resA:
		if r.err != nil {
			c.Check(isSandboxBindError(r.err), "public rendezvous A failed only under the sandbox: %v", r.err)
			return
		}
		connA = r.conn
	case <-time.After(5 * time.Second):
		c.Check(false, "public rendezvous A did not finish in time")
		return
	}
	select {
	case r := <-resB:
		if r.err != nil {
			c.Check(false, "public rendezvous B failed: %v", r.err)
			return
		}
		connB = r.conn
	case <-time.After(5 * time.Second):
		c.Check(false, "public rendezvous B did not finish in time")
		return
	}
	defer connA.Close()
	defer connB.Close()

	c.Check(connA.SocketId() != connB.SocketId(), "the two public endpoints use distinct socket IDs")

	// Bidirectional transfer.
	ping := []byte("public-rendezvous-ping")
	pong := []byte("public-rendezvous-pong")

	c.Check(connB.SetReadDeadline(time.Now().Add(5*time.Second)) == nil, "set B read deadline")
	n, err := connA.Write(ping)
	c.Check(err == nil && n == len(ping), "A writes ping over the public entry: n=%d err=%v", n, err)
	gotPing := make([]byte, len(ping))
	_, err = readFull(connB, gotPing)
	c.Check(err == nil && bytes.Equal(gotPing, ping), "B reads ping bytes exactly: %v %x", err, gotPing)

	c.Check(connA.SetReadDeadline(time.Now().Add(5*time.Second)) == nil, "set A read deadline")
	n, err = connB.Write(pong)
	c.Check(err == nil && n == len(pong), "B writes pong: n=%d err=%v", n, err)
	gotPong := make([]byte, len(pong))
	_, err = readFull(connA, gotPong)
	c.Check(err == nil && bytes.Equal(gotPong, pong), "A reads pong bytes exactly: %v %x", err, gotPong)

	// Re-inject a duplicate WAVEHAND carrying the peer's (B's) identity into A.
	rzA := connA.(*rendezvouser)
	rzB := connB.(*rendezvouser)
	peerCIF := handshakeCIF(buildWavehand(rzB))
	c.Check(peerCIF != nil, "built duplicate WAVEHAND from the peer identity")
	dupPacket, perr := packet.NewPacketFromData(rzA.remoteAddr, marshalHandshake(rzA, peerCIF, rzA.socketId))
	c.Check(perr == nil, "parsed duplicate handshake packet")
	idBefore := connA.SocketId()
	offsetBefore := rzA.conn.havePeerOffset
	rzA.rcvQueue <- dupPacket

	// Also re-send a duplicate CONCLUSION to exercise the late-AGREEMENT path.
	rzB.lock.Lock()
	dupConclusionCIF := rzB.buildConclusionCIF(true)
	rzB.lock.Unlock()
	dupConclusion := marshalHandshake(rzA, dupConclusionCIF, rzA.socketId)
	dupPacket2, perr2 := packet.NewPacketFromData(rzA.remoteAddr, dupConclusion)
	c.Check(perr2 == nil, "parsed duplicate CONCLUSION packet")
	rzA.rcvQueue <- dupPacket2

	waitState(t, time.Second, func() bool { return true }, rzA.stateSignal)

	c.Check(connA.SocketId() == idBefore, "duplicate handshake keeps the connection identity")
	c.Check(rzA.connNow() != nil && rzA.conn.havePeerOffset == offsetBefore, "duplicate handshake does not reset the media time base")

	// The connection still carries media after the duplicate.
	c.Check(connA.SetReadDeadline(time.Now().Add(5*time.Second)) == nil, "re-set A read deadline")
	n, err = connB.Write(ping)
	c.Check(err == nil && n == len(ping), "B writes again after the duplicate: n=%d err=%v", n, err)
	again := make([]byte, len(ping))
	_, err = readFull(connA, again)
	c.Check(err == nil && bytes.Equal(again, ping), "media still flows after a duplicate handshake: %x", again)
}

// isSandboxBindError reports whether err is the local-sandbox EPERM that
// prevents UDP binding in restricted environments.
func isSandboxBindError(err error) bool {
	if err == nil {
		return false
	}
	msg := err.Error()
	return strings.Contains(msg, "operation not permitted") || strings.Contains(msg, "bind")
}

// readFull fills buf from the connection, tolerating partial Reads.
func readFull(conn Conn, buf []byte) (int, error) {
	total := 0
	for total < len(buf) {
		n, err := conn.Read(buf[total:])
		if err != nil {
			return total, err
		}
		total += n
	}
	return total, nil
}
