package rtp

import (
	"encoding/binary"
	"math"
	"net"
	"sync"
	"testing"
	"time"

	"github.com/wernerd/GoRTP/src/net/rtp/v2/task_support/taskcheck"
)

/*
 * Tests for the RFC 3611 XR receiver reference time (RRTR) / DLRR round trip
 * time measurement. Sessions exchange real RTCP compounds through a
 * controllable TransportWrite; a fixed injected clock makes RRTR, DLRR and
 * RTT values exactly reproducible.
 */

// xrTestClock is a manually advanced clock injected via SetXRClock.
type xrTestClock struct {
	mu  sync.Mutex
	now time.Time
}

func newXRTestClock(now time.Time) *xrTestClock {
	return &xrTestClock{now: now}
}

func (c *xrTestClock) Now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.now
}

func (c *xrTestClock) Advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now = c.now.Add(d)
}

func (c *xrTestClock) Set(t time.Time) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.now = t
}

// xrTestTransport captures written RTCP compounds instead of sending them.
type xrTestTransport struct {
	mu       sync.Mutex
	end      TransportEnd
	ctrlSent [][]byte
	dataSent int
}

func (tp *xrTestTransport) ListenOnTransports() error        { return nil }
func (tp *xrTestTransport) OnRecvData(rp *DataPacket) bool   { return false }
func (tp *xrTestTransport) OnRecvCtrl(rp *CtrlPacket) bool   { return false }
func (tp *xrTestTransport) SetCallUpper(upper TransportRecv) {}
func (tp *xrTestTransport) SetToLower(lower TransportWrite)  {}
func (tp *xrTestTransport) CloseWrite()                      {}
func (tp *xrTestTransport) SetEndChannel(ch TransportEnd)    { tp.end = ch }
func (tp *xrTestTransport) CloseRecv() {
	if tp.end != nil {
		tp.end <- DataTransportRecvStopped
		tp.end <- CtrlTransportRecvStopped
	}
}

func (tp *xrTestTransport) WriteDataTo(rp *DataPacket, addr *Address) (int, error) {
	tp.mu.Lock()
	tp.dataSent++
	tp.mu.Unlock()
	return 0, nil
}

func (tp *xrTestTransport) WriteCtrlTo(rp *CtrlPacket, addr *Address) (int, error) {
	buf := append([]byte(nil), rp.Buffer()[:rp.InUse()]...)
	tp.mu.Lock()
	tp.ctrlSent = append(tp.ctrlSent, buf)
	tp.mu.Unlock()
	return len(buf), nil
}

func (tp *xrTestTransport) popCtrl() []byte {
	tp.mu.Lock()
	defer tp.mu.Unlock()
	if len(tp.ctrlSent) == 0 {
		return nil
	}
	raw := tp.ctrlSent[0]
	tp.ctrlSent = tp.ctrlSent[1:]
	return raw
}

func (tp *xrTestTransport) dataCount() int {
	tp.mu.Lock()
	defer tp.mu.Unlock()
	return tp.dataSent
}

var (
	xrAddrA = Address{IPAddr: net.IPv4(127, 0, 0, 10), DataPort: 16100, CtrlPort: 16101}
	xrAddrB = Address{IPAddr: net.IPv4(127, 0, 0, 11), DataPort: 16200, CtrlPort: 16201}
	xrAddrC = Address{IPAddr: net.IPv4(127, 0, 0, 12), DataPort: 16300, CtrlPort: 16301}
	xrAddrD = Address{IPAddr: net.IPv4(127, 0, 0, 13), DataPort: 16400, CtrlPort: 16401}
)

// newXRTestSession creates a session with one output stream and an injected
// clock. XR is not enabled here; tests call EnableXR explicitly.
func newXRTestSession(clock *xrTestClock, ssrc uint32, own *Address) (*Session, *xrTestTransport) {
	tp := &xrTestTransport{}
	s := NewSession(tp, tp)
	s.SetXRClock(clock.Now)
	idx, err := s.NewSsrcStreamOut(own, ssrc, 1000)
	if err != "" {
		panic("NewSsrcStreamOut failed: " + string(err))
	}
	s.SsrcStreamOutForIndex(idx).SetPayloadType(0)
	s.AddRemote(&Address{IPAddr: net.IPv4(127, 0, 0, 1), DataPort: 17000, CtrlPort: 17001})
	s.rtcpServiceActive = true
	// Keep manually stepped reports from timing out input streams.
	s.rtcpDataTimeout = math.MaxInt64
	s.rtcpSsrcTimeout = math.MaxInt64
	return s, tp
}

// xrStepReport runs one report step and returns the emitted compound.
func xrStepReport(s *Session, tp *xrTestTransport) []byte {
	s.rtcpReportStep(time.Now().UnixNano())
	return tp.popCtrl()
}

// xrDeliver feeds a received compound into a session.
func xrDeliver(s *Session, raw []byte, from *Address) bool {
	rp := &CtrlPacket{}
	rp.buffer = make([]byte, defaultBufferSize) // full size: the packet is recycled into the free list
	rp.inUse = copy(rp.buffer, raw)
	rp.fromAddr = *from
	return s.OnRecvCtrl(rp)
}

// --- independent wire parsing helpers ---

type xrBlockView struct {
	bt   int
	body []byte
}

type xrPacketView struct {
	offset int
	ssrc   uint32
	blocks []xrBlockView
}

// xrFindPackets walks a compound and decodes every XR packet it finds.
func xrFindPackets(raw []byte) []xrPacketView {
	var out []xrPacketView
	for off := 0; off+4 <= len(raw); {
		pktLen := (int(binary.BigEndian.Uint16(raw[off+2:])) + 1) * 4
		if raw[off+1] == byte(RtcpXr) {
			v := xrPacketView{offset: off, ssrc: binary.BigEndian.Uint32(raw[off+4:])}
			for blk := off + 8; blk+4 <= off+pktLen; {
				blkLen := int(binary.BigEndian.Uint16(raw[blk+2:])) * 4
				v.blocks = append(v.blocks, xrBlockView{int(raw[blk]), raw[blk+4 : blk+4+blkLen]})
				blk += 4 + blkLen
			}
			out = append(out, v)
		}
		off += pktLen
	}
	return out
}

// xrCompoundTypes lists the RTCP packet types of a compound in order.
func xrCompoundTypes(raw []byte) []int {
	var types []int
	for off := 0; off+4 <= len(raw); {
		pktLen := (int(binary.BigEndian.Uint16(raw[off+2:])) + 1) * 4
		types = append(types, int(raw[off+1]))
		off += pktLen
	}
	return types
}

func xrViewRRTR(v xrPacketView) (sec, frac uint32, ok bool) {
	for _, b := range v.blocks {
		if b.bt == xrBlockRRTR && len(b.body) == 8 {
			return binary.BigEndian.Uint32(b.body), binary.BigEndian.Uint32(b.body[4:]), true
		}
	}
	return 0, 0, false
}

func xrViewDlrrs(v xrPacketView) []xrDlrrSub {
	var subs []xrDlrrSub
	for _, b := range v.blocks {
		if b.bt != xrBlockDLRR {
			continue
		}
		for p := 0; p+12 <= len(b.body); p += 12 {
			subs = append(subs, xrDlrrSub{
				ssrc: binary.BigEndian.Uint32(b.body[p:]),
				lrr:  binary.BigEndian.Uint32(b.body[p+4:]),
				dlrr: binary.BigEndian.Uint32(b.body[p+8:]),
			})
		}
	}
	return subs
}

func xrFindSub(subs []xrDlrrSub, ssrc uint32) (xrDlrrSub, bool) {
	for _, s := range subs {
		if s.ssrc == ssrc {
			return s, true
		}
	}
	return xrDlrrSub{}, false
}

// --- crafting helpers for hand-built peer compounds ---

func xrCraftBlock(bt byte, body []byte) []byte {
	b := make([]byte, 4+len(body))
	b[0] = bt
	binary.BigEndian.PutUint16(b[2:], uint16(len(body)/4))
	copy(b[4:], body)
	return b
}

func xrRRTRBody(sec, frac uint32) []byte {
	body := make([]byte, 8)
	binary.BigEndian.PutUint32(body, sec)
	binary.BigEndian.PutUint32(body[4:], frac)
	return body
}

// xrCraftCompound builds "RR + XR(blocks...)" for the given reporter SSRC.
func xrCraftCompound(reporter uint32, blocks ...[]byte) []byte {
	body := make([]byte, 4)
	binary.BigEndian.PutUint32(body, reporter)
	for _, blk := range blocks {
		body = append(body, blk...)
	}
	xr := make([]byte, 4)
	xr[0] = 0x80
	xr[1] = byte(RtcpXr)
	binary.BigEndian.PutUint16(xr[2:], uint16(len(body)/4)) // header word plus body, minus one
	rr := []byte{0x80, byte(RtcpRR), 0, 1,
		byte(reporter >> 24), byte(reporter >> 16), byte(reporter >> 8), byte(reporter)}
	return append(append(rr, xr...), body...)
}

func TestXRWire(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newXRTestClock(time.Unix(1700000000, 0))
		sess, tp := newXRTestSession(clock, 0xAAAA0001, &xrAddrA)
		sess.EnableXR(true)

		// --- Output format: RR + SDES + XR with a single RRTR block ---
		raw := xrStepReport(sess, tp)
		c.Check(raw != nil, "no compound produced by report step")
		types := xrCompoundTypes(raw)
		c.Check(len(types) == 3 && types[0] == RtcpRR && types[1] == RtcpSdes && types[2] == RtcpXr,
			"unexpected compound layout: %v", types)
		xrs := xrFindPackets(raw)
		c.Check(len(xrs) == 1, "expected exactly one XR packet, got %d", len(xrs))
		if len(xrs) != 1 {
			return
		}
		x := xrs[0]
		off := x.offset
		c.Check(raw[off] == 0x80, "XR header: V=2, no padding, zero count expected, got %02x", raw[off])
		c.Check(x.ssrc == 0xAAAA0001, "XR sender SSRC mismatch: %08x", x.ssrc)
		pktLen := (int(binary.BigEndian.Uint16(raw[off+2:])) + 1) * 4
		c.Check(off+pktLen == len(raw), "XR length field does not cover rest of compound")
		c.Check(len(x.blocks) == 1 && x.blocks[0].bt == xrBlockRRTR,
			"XR without peers must carry only RRTR, got %d blocks", len(x.blocks))
		sec, frac, ok := xrViewRRTR(x)
		c.Check(ok, "RRTR block missing")
		wantSec, wantFrac := toNtpStamp(clock.Now().UnixNano())
		c.Check(sec == wantSec && frac == wantFrac,
			"RRTR NTP mismatch: got %08x/%08x want %08x/%08x", sec, frac, wantSec, wantFrac)

		// --- A peer reference is answered with a DLRR ---
		peer := uint32(0xBBBB0002)
		ref := xrCraftCompound(peer, xrCraftBlock(xrBlockRRTR, xrRRTRBody(0x11223344, 0x55667788)))
		c.Check(xrDeliver(sess, ref, &xrAddrB), "peer RRTR compound rejected")
		clock.Advance(250 * time.Millisecond)
		raw = xrStepReport(sess, tp)
		xrs = xrFindPackets(raw)
		c.Check(len(xrs) == 1, "expected one XR packet after peer ref")
		subs := xrViewDlrrs(xrs[0])
		sub, found := xrFindSub(subs, peer)
		c.Check(found, "no DLRR sub-block for peer")
		c.Check(sub.lrr == xrCompact(0x11223344, 0x55667788),
			"DLRR LRR mismatch: got %08x", sub.lrr)
		c.Check(sub.dlrr == uint32(xrDurationUnits(250*time.Millisecond)),
			"DLRR delay mismatch: got %d want %d", sub.dlrr, xrDurationUnits(250*time.Millisecond))

		// --- The same reference value again must not restart the wait ---
		c.Check(xrDeliver(sess, ref, &xrAddrB), "duplicate peer RRTR rejected")
		clock.Advance(250 * time.Millisecond)
		raw = xrStepReport(sess, tp)
		sub, found = xrFindSub(xrViewDlrrs(xrFindPackets(raw)[0]), peer)
		c.Check(found, "no DLRR sub-block after duplicate ref")
		c.Check(sub.dlrr == uint32(xrDurationUnits(500*time.Millisecond)),
			"duplicate ref restarted the wait: got %d want %d", sub.dlrr, xrDurationUnits(500*time.Millisecond))

		// --- A newer reference replaces the stored one ---
		refNew := xrCraftCompound(peer, xrCraftBlock(xrBlockRRTR, xrRRTRBody(0x11223345, 0x55667788)))
		c.Check(xrDeliver(sess, refNew, &xrAddrB), "newer peer RRTR rejected")
		clock.Advance(100 * time.Millisecond)
		raw = xrStepReport(sess, tp)
		sub, found = xrFindSub(xrViewDlrrs(xrFindPackets(raw)[0]), peer)
		c.Check(found && sub.lrr == xrCompact(0x11223345, 0x55667788),
			"newest peer ref not recorded: found=%v lrr=%08x", found, sub.lrr)
		c.Check(sub.dlrr == uint32(xrDurationUnits(100*time.Millisecond)),
			"DLRR after new ref mismatch: got %d", sub.dlrr)

		// --- Unknown blocks are skipped, empty DLRR is permitted ---
		peer2 := uint32(0xBBBB0009)
		mixed := xrCraftCompound(peer2,
			xrCraftBlock(77, []byte{1, 2, 3, 4}),
			xrCraftBlock(xrBlockRRTR, xrRRTRBody(0x22223333, 0x44445555)),
			xrCraftBlock(xrBlockDLRR, nil))
		c.Check(xrDeliver(sess, mixed, &xrAddrB), "compound with unknown XR block rejected")
		clock.Advance(10 * time.Millisecond)
		raw = xrStepReport(sess, tp)
		sub, found = xrFindSub(xrViewDlrrs(xrFindPackets(raw)[0]), peer2)
		c.Check(found && sub.lrr == xrCompact(0x22223333, 0x44445555),
			"peer ref behind unknown block not recorded")
		c.Check(found && sub.dlrr == uint32(xrDurationUnits(10*time.Millisecond)),
			"DLRR wait after unknown block mismatch: got %d", sub.dlrr)

		// --- Corrupt packets are rejected without committing XR state ---
		sess2, tp2 := newXRTestSession(clock, 0xAAAA0005, &xrAddrA)
		sess2.EnableXR(true)
		peer3 := uint32(0xBBBB000A)

		badRRTR := xrCraftCompound(peer3, xrCraftBlock(xrBlockRRTR, make([]byte, 12)))
		c.Check(!xrDeliver(sess2, badRRTR, &xrAddrB), "RRTR with length 3 must be rejected")

		badDLRR := xrCraftCompound(peer3, xrCraftBlock(xrBlockDLRR, make([]byte, 16)))
		c.Check(!xrDeliver(sess2, badDLRR, &xrAddrB), "DLRR length not divisible by 3 must be rejected")

		// XR packet whose length field reaches beyond the datagram.
		truncated := xrCraftCompound(peer3, xrCraftBlock(xrBlockRRTR, xrRRTRBody(1, 2)))
		truncated = truncated[:len(truncated)-4]
		c.Check(!xrDeliver(sess2, truncated, &xrAddrB), "truncated XR packet must be rejected")

		// XR header only, no room for the sender SSRC.
		tiny := []byte{0x80, byte(RtcpRR), 0, 1, 0xBB, 0xBB, 0x00, 0x0A, 0x80, byte(RtcpXr), 0, 0}
		c.Check(!xrDeliver(sess2, tiny, &xrAddrB), "truncated XR header must be rejected")

		// Valid RRTR followed by a corrupt trailing block: the datagram is
		// rejected and even the valid block must not leave state behind.
		badTail := xrCraftCompound(peer3,
			xrCraftBlock(xrBlockRRTR, xrRRTRBody(0x33334444, 0x55556666)),
			[]byte{90, 0, 0, 10}) // unknown block declaring 40 bytes that are not there
		c.Check(!xrDeliver(sess2, badTail, &xrAddrB), "corrupt trailing block must be rejected")
		raw2 := xrStepReport(sess2, tp2)
		xrs2 := xrFindPackets(raw2)
		c.Check(len(xrs2) == 1, "expected one XR packet from sess2")
		_, found = xrFindSub(xrViewDlrrs(xrs2[0]), peer3)
		c.Check(!found, "corrupt datagram left partial XR state behind")

		// --- Normal final RTCP padding is accepted when it fits ---
		peer4 := uint32(0xBBBB000B)
		padBody := make([]byte, 4+12+4)
		binary.BigEndian.PutUint32(padBody, peer4)
		copy(padBody[4:], xrCraftBlock(xrBlockRRTR, xrRRTRBody(0x77778888, 0x9999AAAA)))
		padBody[len(padBody)-1] = 4 // padding count
		padded := []byte{0x80, byte(RtcpRR), 0, 1, 0xBB, 0xBB, 0x00, 0x0B,
			0xA0, byte(RtcpXr), 0, byte(len(padBody) / 4)}
		padded = append(padded, padBody...)
		c.Check(xrDeliver(sess2, padded, &xrAddrB), "valid padded XR rejected")
		clock.Advance(5 * time.Millisecond)
		raw2 = xrStepReport(sess2, tp2)
		sub, found = xrFindSub(xrViewDlrrs(xrFindPackets(raw2)[0]), peer4)
		c.Check(found && sub.lrr == xrCompact(0x77778888, 0x9999AAAA),
			"ref from padded XR not recorded")
		c.Check(found && sub.dlrr == uint32(xrDurationUnits(5*time.Millisecond)),
			"DLRR wait for padded ref mismatch: got %d", sub.dlrr)

		// Padding count that does not fit the packet is rejected.
		badPadBody := make([]byte, 4+12+4)
		binary.BigEndian.PutUint32(badPadBody, peer4)
		copy(badPadBody[4:], xrCraftBlock(xrBlockRRTR, xrRRTRBody(0x11111111, 0x22222222)))
		badPadBody[len(badPadBody)-1] = 24 // larger than the packet allows
		badPad := []byte{0x80, byte(RtcpRR), 0, 1, 0xBB, 0xBB, 0x00, 0x0B,
			0xA0, byte(RtcpXr), 0, byte(len(badPadBody) / 4)}
		badPad = append(badPad, badPadBody...)
		c.Check(!xrDeliver(sess2, badPad, &xrAddrB), "oversized padding count must be rejected")

		// --- XR disabled: no XR on the wire, incoming XR skipped ---
		sess3, tp3 := newXRTestSession(clock, 0xAAAA0006, &xrAddrA)
		raw3 := xrStepReport(sess3, tp3)
		c.Check(len(xrFindPackets(raw3)) == 0, "XR emitted while disabled")
		c.Check(xrDeliver(sess3, badRRTR, &xrAddrB), "XR-disabled session must skip XR packets")
	})
}

func TestXRRoundTrip(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newXRTestClock(time.Unix(1700000000, 0))
		sessA, tpA := newXRTestSession(clock, 0xAAAA0001, &xrAddrA)
		sessA.EnableXR(true)
		sessB, tpB := newXRTestSession(clock, 0xBBBB0002, &xrAddrB)
		sessB.EnableXR(true)
		sessC, tpC := newXRTestSession(clock, 0xCCCC0003, &xrAddrC)
		sessC.EnableXR(true)
		aSsrc, bSsrc, cSsrc := uint32(0xAAAA0001), uint32(0xBBBB0002), uint32(0xCCCC0003)

		_, ok := sessA.XRRTT(aSsrc, bSsrc)
		c.Check(!ok, "RTT must not exist before any exchange")

		// --- Media path still works; B receives but never sends RTP ---
		dataChanB := sessB.CreateDataReceiveChan()
		rp := sessA.NewDataPacket(160)
		rp.SetPayload(make([]byte, 160))
		sessA.WriteData(rp)
		rp.fromAddr = xrAddrA
		c.Check(sessB.OnRecvData(rp), "B rejected RTP data")
		select {
		case pkt := <-dataChanB:
			pkt.FreePacket()
		default:
			c.Check(false, "B did not forward RTP to the application")
		}
		c.Check(tpA.dataCount() == 1, "A's RTP packet not written to transport")
		c.Check(tpB.dataCount() == 0, "receiver B must not send RTP")

		// --- t=0: A reports (SR because A sent RTP) ---
		s1 := xrStepReport(sessA, tpA)
		types := xrCompoundTypes(s1)
		c.Check(len(types) == 3 && types[0] == RtcpSR && types[1] == RtcpSdes && types[2] == RtcpXr,
			"sender compound layout unexpected: %v", types)
		xrs := xrFindPackets(s1)
		c.Check(len(xrs) == 1 && xrs[0].ssrc == aSsrc, "XR identity must be the reporter's SSRC")
		secA0, fracA0, ok := xrViewRRTR(xrs[0])
		c.Check(ok, "RRTR missing in A's compound")
		lrrA0 := xrCompact(secA0, fracA0)
		c.Check(xrDeliver(sessB, s1, &xrAddrA), "B rejected A's compound")
		c.Check(xrDeliver(sessC, s1, &xrAddrA), "C rejected A's compound")

		// --- t=100ms: B and C answer; both are pure receivers (RR first) ---
		clock.Advance(100 * time.Millisecond)
		b1 := xrStepReport(sessB, tpB)
		c1 := xrStepReport(sessC, tpC)
		c.Check(xrCompoundTypes(b1)[0] == RtcpRR, "receiver B must send RR, got %d", xrCompoundTypes(b1)[0])
		c.Check(tpB.dataCount() == 0, "receiver B sent RTP during exchange")
		subB1, found := xrFindSub(xrViewDlrrs(xrFindPackets(b1)[0]), aSsrc)
		c.Check(found && subB1.lrr == lrrA0, "B's DLRR does not answer A's reference")
		c.Check(subB1.dlrr == uint32(xrDurationUnits(100*time.Millisecond)),
			"B's DLRR wait mismatch: got %d", subB1.dlrr)
		subC1, found := xrFindSub(xrViewDlrrs(xrFindPackets(c1)[0]), aSsrc)
		c.Check(found && subC1.dlrr == subB1.dlrr, "C's DLRR mismatch")

		// C's answer arrives at A without any further delay: zero RTT is valid.
		c.Check(xrDeliver(sessA, c1, &xrAddrC), "A rejected C's compound")
		rtt, ok := sessA.XRRTT(aSsrc, cSsrc)
		c.Check(ok, "zero-delay RTT must be a valid result")
		c.Check(rtt == 0, "zero-delay RTT must be zero, got %v", rtt)

		// --- t=140ms: B's answer arrives at A after 40ms of transport ---
		clock.Advance(40 * time.Millisecond)
		c.Check(xrDeliver(sessA, b1, &xrAddrB), "A rejected B's compound")
		wantAB := time.Duration((xrDurationUnits(140*time.Millisecond) - int64(subB1.dlrr)) * 1e9 / 65536)
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok, "no RTT for A->B association")
		c.Check(rtt == wantAB, "RTT(A,B) mismatch: got %v want %v", rtt, wantAB)
		// B's answer must not consume C's association.
		rtt, ok = sessA.XRRTT(aSsrc, cSsrc)
		c.Check(ok && rtt == 0, "B's feedback disturbed A->C association: %v %v", rtt, ok)

		// A's report now answers B's reference; B learns its own RTT.
		a2 := xrStepReport(sessA, tpA)
		subA2, found := xrFindSub(xrViewDlrrs(xrFindPackets(a2)[0]), bSsrc)
		c.Check(found && subA2.dlrr == 0, "A's DLRR for B should have zero wait, got %d", subA2.dlrr)
		c.Check(xrDeliver(sessB, a2, &xrAddrA), "B rejected A's second compound")
		wantBA := time.Duration(xrDurationUnits(40*time.Millisecond) * 1e9 / 65536)
		rtt, ok = sessB.XRRTT(bSsrc, aSsrc)
		c.Check(ok && rtt == wantBA, "RTT(B,A) mismatch: got %v want %v (%v)", rtt, wantBA, ok)

		// --- Duplicate and late feedback ---
		clock.Advance(20 * time.Millisecond) // t=160
		a3 := xrStepReport(sessA, tpA)
		secA3, fracA3, _ := xrViewRRTR(xrFindPackets(a3)[0])
		lrrA3 := xrCompact(secA3, fracA3)
		c.Check(xrDeliver(sessB, a3, &xrAddrA), "B rejected A's third compound")
		clock.Advance(20 * time.Millisecond) // t=180
		b2 := xrStepReport(sessB, tpB)       // answers A's t=160 reference, hold it back
		clock.Advance(20 * time.Millisecond) // t=200
		a4 := xrStepReport(sessA, tpA)
		c.Check(xrDeliver(sessB, a4, &xrAddrA), "B rejected A's fourth compound")
		clock.Advance(20 * time.Millisecond) // t=220
		b3 := xrStepReport(sessB, tpB)       // answers A's t=200 reference
		clock.Advance(20 * time.Millisecond) // t=240

		// The next round's reference (t=200) is already out, but the answer to
		// the previous round (t=160) still matches.
		c.Check(xrDeliver(sessA, b2, &xrAddrB), "A rejected B's delayed answer")
		subB2, _ := xrFindSub(xrViewDlrrs(xrFindPackets(b2)[0]), aSsrc)
		c.Check(subB2.lrr == lrrA3, "B2 answers unexpected reference: %08x", subB2.lrr)
		wantB2 := time.Duration((xrDurationUnits(80*time.Millisecond) - int64(subB2.dlrr)) * 1e9 / 65536)
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok && rtt == wantB2, "previous round answer did not match: got %v want %v", rtt, wantB2)

		// The newer round's answer updates the result.
		c.Check(xrDeliver(sessA, b3, &xrAddrB), "A rejected B's current answer")
		subB3, _ := xrFindSub(xrViewDlrrs(xrFindPackets(b3)[0]), aSsrc)
		wantB3 := time.Duration((xrDurationUnits(40*time.Millisecond) - int64(subB3.dlrr)) * 1e9 / 65536)
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok && rtt == wantB3, "new round answer mismatch: got %v want %v", rtt, wantB3)

		// A late answer to the old reference must not overwrite the newer result.
		c.Check(xrDeliver(sessA, b2, &xrAddrB), "A rejected re-delivered old answer")
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok && rtt == wantB3, "late old answer overwrote newer result: got %v want %v", rtt, wantB3)
		// A duplicate of the current answer must not change anything either.
		c.Check(xrDeliver(sessA, b3, &xrAddrB), "A rejected duplicate answer")
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok && rtt == wantB3, "duplicate answer changed result: got %v want %v", rtt, wantB3)

		// --- References older than one minute are not eligible ---
		clock.Advance(61 * time.Second) // t=61240ms
		b4 := xrStepReport(sessB, tpB)
		c.Check(xrDeliver(sessA, b4, &xrAddrB), "A rejected stale answer")
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok && rtt == wantB3, "stale reference produced a result: got %v want %v", rtt, wantB3)

		// --- Unmatched and negative feedback produce nothing ---
		a5 := xrStepReport(sessA, tpA)
		secA5, fracA5, _ := xrViewRRTR(xrFindPackets(a5)[0])
		lrrA5 := xrCompact(secA5, fracA5)

		negBody := make([]byte, 12)
		binary.BigEndian.PutUint32(negBody, aSsrc)
		binary.BigEndian.PutUint32(negBody[4:], lrrA5)
		binary.BigEndian.PutUint32(negBody[8:], 0xFFFFFF00) // wait far beyond the elapsed time
		neg := xrCraftCompound(bSsrc, xrCraftBlock(xrBlockDLRR, negBody))
		c.Check(xrDeliver(sessA, neg, &xrAddrB), "A rejected negative-RTT compound")
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok && rtt == wantB3, "negative feedback produced a result: got %v want %v", rtt, wantB3)

		binary.BigEndian.PutUint32(negBody[4:], 0xDEADBEEF) // reference never sent
		binary.BigEndian.PutUint32(negBody[8:], 0)
		unmatched := xrCraftCompound(bSsrc, xrCraftBlock(xrBlockDLRR, negBody))
		c.Check(xrDeliver(sessA, unmatched, &xrAddrB), "A rejected unmatched compound")
		rtt, ok = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(ok && rtt == wantB3, "unmatched feedback produced a result: got %v want %v", rtt, wantB3)

		// --- Compact NTP wrap-around must not become a huge delay ---
		sessA2, tpA2 := newXRTestSession(clock, 0xAAAA1001, &xrAddrA)
		sessA2.EnableXR(true)
		sessB2, tpB2 := newXRTestSession(clock, 0xBBBB1002, &xrAddrB)
		sessB2.EnableXR(true)
		a2Ssrc, b2Ssrc := uint32(0xAAAA1001), uint32(0xBBBB1002)

		// Place the clock so the middle 32 bits of the NTP timestamp wrap.
		// (NTP seconds are Unix seconds plus 2208988800; 0x817F + 0x7E80 = 0xFFFF.)
		clock.Set(time.Unix(0x0000817F, (int64(0xFFFF0000)*1e9)>>32))
		w0 := xrStepReport(sessA2, tpA2)
		secW0, fracW0, _ := xrViewRRTR(xrFindPackets(w0)[0])
		compact0 := xrCompact(secW0, fracW0)
		c.Check(xrDeliver(sessB2, w0, &xrAddrA), "B2 rejected pre-wrap reference")
		clock.Advance(1 * time.Millisecond)
		bw := xrStepReport(sessB2, tpB2)
		clock.Advance(1 * time.Millisecond)
		w1 := xrStepReport(sessA2, tpA2)
		secW1, fracW1, _ := xrViewRRTR(xrFindPackets(w1)[0])
		compact1 := xrCompact(secW1, fracW1)
		c.Check(compact1 < compact0, "compact NTP did not wrap: %08x -> %08x", compact0, compact1)

		c.Check(xrDeliver(sessA2, bw, &xrAddrB), "A2 rejected wrap-around answer")
		subW, _ := xrFindSub(xrViewDlrrs(xrFindPackets(bw)[0]), a2Ssrc)
		c.Check(subW.lrr == compact0, "wrap answer LRR mismatch: %08x", subW.lrr)
		wantW := time.Duration((xrDurationUnits(2*time.Millisecond) - int64(subW.dlrr)) * 1e9 / 65536)
		rtt, ok = sessA2.XRRTT(a2Ssrc, b2Ssrc)
		c.Check(ok && rtt == wantW, "wrap-around RTT mismatch: got %v want %v", rtt, wantW)
		c.Check(rtt >= 0 && rtt < 5*time.Millisecond, "wrap-around produced huge delay: %v", rtt)

		// --- Concurrent queries while receiving must not race ---
		done := make(chan struct{})
		go func() {
			defer close(done)
			for i := 0; i < 200; i++ {
				sessA.XRRTT(aSsrc, bSsrc)
			}
		}()
		clock.Advance(time.Millisecond)
		c.Check(xrDeliver(sessA, xrStepReport(sessB, tpB), &xrAddrB), "A rejected compound during concurrent queries")
		<-done
	})
}

func TestXRLifecycle(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		clock := newXRTestClock(time.Unix(1700000000, 0))
		sessA, tpA := newXRTestSession(clock, 0xAAAA0001, &xrAddrA)
		sessA.EnableXR(true)
		sessB, tpB := newXRTestSession(clock, 0xBBBB0002, &xrAddrB)
		sessB.EnableXR(true)
		aSsrc, bSsrc := uint32(0xAAAA0001), uint32(0xBBBB0002)

		// Establish associations in both directions.
		a1 := xrStepReport(sessA, tpA)
		c.Check(xrDeliver(sessB, a1, &xrAddrA), "B rejected A's compound")
		clock.Advance(10 * time.Millisecond)
		b1 := xrStepReport(sessB, tpB)
		c.Check(xrDeliver(sessA, b1, &xrAddrB), "A rejected B's compound")
		clock.Advance(10 * time.Millisecond)
		a2 := xrStepReport(sessA, tpA)
		c.Check(xrDeliver(sessB, a2, &xrAddrA), "B rejected A's second compound")
		_, okA := sessA.XRRTT(aSsrc, bSsrc)
		_, okB := sessB.XRRTT(bSsrc, aSsrc)
		c.Check(okA && okB, "associations not established: A=%v B=%v", okA, okB)

		// --- Local stream close drops the local associations ---
		sessB.SsrcStreamCloseForIndex(0)
		_, okB = sessB.XRRTT(bSsrc, aSsrc)
		c.Check(!okB, "local stream close must drop the local RTT")

		// --- The BYE from B clears A's associations for B ---
		bye := tpB.popCtrl() // the BYE compound emitted by SsrcStreamCloseForIndex
		c.Check(bye != nil, "stream close did not emit a BYE compound")
		c.Check(xrDeliver(sessA, bye, &xrAddrB), "A rejected B's BYE compound")
		_, okA = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(!okA, "peer BYE must drop the RTT association")
		clock.Advance(10 * time.Millisecond)
		a3 := xrStepReport(sessA, tpA) // also moves B's input stream isClosing -> isClosed
		_, found := xrFindSub(xrViewDlrrs(xrFindPackets(a3)[0]), bSsrc)
		c.Check(!found, "A still answers B's reference after BYE")
		// Purge the closed input stream so a peer reusing the SSRC is accepted again.
		sessA.rtcpReportStep(time.Now().UnixNano()) // isClosed -> removed
		tpA.popCtrl()

		// --- Rebirth with the same SSRC starts without inherited state ---
		sessB.rtcpReportStep(time.Now().UnixNano()) // isClosing -> isClosed
		sessB.rtcpReportStep(time.Now().UnixNano()) // isClosed -> removed
		idx, err := sessB.NewSsrcStreamOut(&xrAddrB, bSsrc, 1000)
		c.Check(err == "", "stream rebirth with same SSRC failed: %v", err)
		c.Check(sessB.SsrcStreamOutForIndex(idx).Ssrc() == bSsrc, "reborn stream lost its SSRC")
		sessB.SsrcStreamOutForIndex(idx).SetPayloadType(0)
		_, okB = sessB.XRRTT(bSsrc, aSsrc)
		c.Check(!okB, "reused SSRC inherited an old RTT")

		clock.Advance(10 * time.Millisecond)
		b2 := xrStepReport(sessB, tpB)
		c.Check(b2 != nil, "reborn stream produced no report")
		c.Check(xrDeliver(sessA, b2, &xrAddrB), "A rejected reborn B's compound")
		// A records the fresh reference and answers it in the next round.
		clock.Advance(10 * time.Millisecond)
		a4 := xrStepReport(sessA, tpA)
		secB2, fracB2, _ := xrViewRRTR(xrFindPackets(b2)[0])
		subA4, found := xrFindSub(xrViewDlrrs(xrFindPackets(a4)[0]), bSsrc)
		c.Check(found, "A does not answer the reborn peer")
		c.Check(subA4.lrr == xrCompact(secB2, fracB2), "A answers a stale reference after rebirth")
		c.Check(subA4.dlrr == uint32(xrDurationUnits(10*time.Millisecond)),
			"DLRR wait after rebirth mismatch: got %d", subA4.dlrr)
		// A fresh round trip can be measured again.
		c.Check(xrDeliver(sessB, a4, &xrAddrA), "reborn B rejected A's compound")
		clock.Advance(10 * time.Millisecond)
		b3 := xrStepReport(sessB, tpB)
		c.Check(xrDeliver(sessA, b3, &xrAddrB), "A rejected reborn B's answer")
		_, okA = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(okA, "no fresh RTT after rebirth")

		// --- Session close drops all XR state ---
		sessA.CloseSession()
		_, okA = sessA.XRRTT(aSsrc, bSsrc)
		c.Check(!okA, "session close must drop RTT results")
		sessA.xrMutex.Lock()
		peers, sent, rtts := len(sessA.xrPeerRefs), len(sessA.xrSentRefs), len(sessA.xrRTTs)
		sessA.xrMutex.Unlock()
		c.Check(peers == 0 && sent == 0 && rtts == 0,
			"session close left XR state: peers=%d sent=%d rtts=%d", peers, sent, rtts)

		// --- The timed RTCP service uses the same report step ---
		sessD, tpD := newXRTestSession(clock, 0xDDDD0004, &xrAddrD)
		sessD.EnableXR(true)
		errStart := sessD.StartSession()
		c.Check(errStart == nil, "StartSession failed: %v", errStart)
		var auto []byte
		for i := 0; i < 200 && auto == nil; i++ {
			auto = tpD.popCtrl()
			if auto == nil {
				time.Sleep(25 * time.Millisecond)
			}
		}
		c.Check(auto != nil, "timed RTCP service did not emit a report")
		if auto != nil {
			xrs := xrFindPackets(auto)
			c.Check(len(xrs) == 1 && xrs[0].ssrc == 0xDDDD0004,
				"timed service compound has no proper XR packet")
			if len(xrs) == 1 {
				secD, fracD, okD := xrViewRRTR(xrs[0])
				wantSec, wantFrac := toNtpStamp(clock.Now().UnixNano())
				c.Check(okD && secD == wantSec && fracD == wantFrac,
					"timed service RRTR does not use the injected clock")
			}
		}
		sessD.CloseSession()
	})
}
