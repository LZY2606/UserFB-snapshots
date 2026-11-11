// Package capture builds the bundled synthetic capture. It is generated in
// code rather than checked in as a binary blob so tests can assert against the
// exact scenarios it is meant to exhibit.
package capture

import (
	"encoding/json"

	"rtpfreeze/internal/rtproto"
)

// SSRC values used by the sample.
const (
	SSRCVideo uint32 = 0x11110001 // main video, mid=0 rid=h
	SSRCLow   uint32 = 0x22220002 // simulcast low layer, mid=0 rid=l
	SSRCOther uint32 = 0x55550005 // second media line, mid=1
	SSRCRecv  uint32 = 0x99990001 // the feedback sender
	PTVideo          = 96         // reused across every layer switch
)

const baseTimeUS int64 = 1_000_000

// Builder accumulates packets while maintaining the capture index clock.
type Builder struct {
	packets []rtproto.Packet
	cursor  int64
}

// NewBuilder starts at the fixed synthetic time base.
func NewBuilder() *Builder {
	return &Builder{cursor: baseTimeUS}
}

func (b *Builder) add(p rtproto.Packet) {
	p.Index = len(b.packets)
	if p.TimeUS == 0 {
		p.TimeUS = b.cursor
	}
	b.packets = append(b.packets, p)
}

// advance moves the capture clock by d microseconds.
func (b *Builder) advance(d int64) { b.cursor += d }

func u16(v int) *uint16 { x := uint16(v); return &x }

// rtp appends one RTP datagram.
func (b *Builder) rtp(ssrc uint32, seq int, mid, rid string, tseq int, marker bool) {
	h := &rtproto.RTPHeader{
		Version: 2, Marker: marker, PayloadType: PTVideo,
		Sequence: uint16(seq), Timestamp: uint32(b.cursor / 1000), SSRC: ssrc,
	}
	p := rtproto.Packet{
		TimeUS: b.cursor, Direction: rtproto.DirSend, Kind: rtproto.KindRTP,
		RTP: h, MID: mid, RID: rid, RawSize: 1200,
	}
	if tseq >= 0 {
		p.TransportSeq = u16(tseq)
	}
	b.add(p)
}

// rtcp appends one compound RTCP datagram.
func (b *Builder) rtcp(dir rtproto.Direction, msgs ...rtproto.RTCPMessage) {
	b.add(rtproto.Packet{
		TimeUS: b.cursor, Direction: dir, Kind: rtproto.KindRTCP,
		RTCP: msgs, RawSize: 64,
	})
}

func nack(mediaSSRC uint32, ranges ...rtproto.NACKRange) rtproto.RTCPMessage {
	return rtproto.RTCPMessage{
		FMT: rtproto.FMTNACK, PacketType: rtproto.PTFB, SSRC: SSRCRecv,
		MediaSSRC: mediaSSRC, NACK: ranges,
	}
}

func pli(mediaSSRC uint32) rtproto.RTCPMessage {
	return rtproto.RTCPMessage{
		FMT: rtproto.FMTPLI, PacketType: rtproto.PTPSFB, SSRC: SSRCRecv,
		MediaSSRC: mediaSSRC, PLI: true,
	}
}

func fir(mediaSSRC uint32, seq uint8) rtproto.RTCPMessage {
	return rtproto.RTCPMessage{
		FMT: rtproto.FMTFIR, PacketType: rtproto.PTPSFB, SSRC: SSRCRecv,
		MediaSSRC: mediaSSRC, FIR: true, FIRSeq: seq,
	}
}

func twcc(base uint16, count int, refUS int64) rtproto.RTCPMessage {
	deltas := make([]int64, count)
	for i := range deltas {
		deltas[i] = 20000
	}
	return rtproto.RTCPMessage{
		FMT: rtproto.FMTTWCC, PacketType: rtproto.PTFB, SSRC: SSRCRecv,
		TWCC: &rtproto.TWCCFeedback{
			BaseSequence: base, PacketStatus: 1, ReferenceUS: refUS,
			Runs: []rtproto.TWCCRun{{
				FirstTransportSeq: base, Count: count, DeltaMicros: deltas,
			}},
		},
	}
}

func rr(blocks ...rtproto.ReportBlock) rtproto.RTCPMessage {
	return rtproto.RTCPMessage{
		PacketType: rtproto.PTRR, SSRC: SSRCRecv, ReportBlocks: blocks,
	}
}

func block(ssrc uint32, lsr, dlsr uint32) rtproto.ReportBlock {
	return rtproto.ReportBlock{SSRC: ssrc, FractionLost: 10, HighestSeq: 12345,
		Jitter: 3, LSR: lsr, DLSR: dlsr}
}

// ntpFor encodes a capture-clock microsecond as an NTP (msw, lsw) pair. The
// sample deliberately anchors the NTP clock to the capture monotonic clock so
// offline RTT arithmetic stays self-consistent. Integer arithmetic keeps the
// encoding exactly invertible by the analyzer's ntpToUS.
func ntpFor(us int64) (uint32, uint32) {
	secs := uint32(us / 1_000_000)
	frac := uint32((uint64(us%1_000_000)<<32 + 500_000) / 1_000_000)
	return secs, frac
}

func sr(ssrc uint32, us int64) rtproto.RTCPMessage {
	msw, lsw := ntpFor(us)
	return rtproto.RTCPMessage{
		PacketType: rtproto.PTSR, SSRC: ssrc,
		SenderInfo: &rtproto.SenderInfo{
			NTPMSW: msw, NTPLSW: lsw, RTPTimestamp: uint32(us / 1000),
			PacketCount: 42, OctetCount: 50400,
		},
	}
}

func bye(ssrc uint32) rtproto.RTCPMessage {
	return rtproto.RTCPMessage{PacketType: rtproto.PTBYE, SSRC: ssrc, Reason: "ok"}
}

// Build constructs the bundled capture. The scenarios, in order:
//
//  1. sequence wrap (65530 -> 9) with a loss gap at seqs 4-5,
//  2. overlapping TWCC feedback windows (100-110 vs 105-113),
//  3. simulcast layer switch to rid=l and back, reusing payload type 96,
//  4. a 400 ms encoder silence,
//  5. BYE then SSRC rebirth plus a late NACK for an old generation,
//  6. a PLI storm,
//  7. ICE restart with transport sequence reset,
//  8. an SSRC collision on a second media line,
//  9. receiver reports with a good LSR, LSR=0 and an unmatched LSR (the
//     sender reports for the low layer and the second line are missing).
func Build() []rtproto.Packet {
	b := NewBuilder()

	// --- Phase 1: high layer, sequence wraps 65535 -> 0 --------------------
	for seq := 65530; seq <= 65535; seq++ {
		b.rtp(SSRCVideo, seq, "0", "h", 100+(seq-65530), seq == 65535)
		b.advance(20_000)
	}
	for seq := 0; seq <= 3; seq++ {
		b.rtp(SSRCVideo, seq, "0", "h", 106+seq, false)
		b.advance(20_000)
	}
	// Seqs 4 and 5 are lost on the wire: no packets, sequence jumps 3 -> 6.
	for seq := 6; seq <= 9; seq++ {
		b.rtp(SSRCVideo, seq, "0", "h", 104+seq, seq == 9)
		b.advance(20_000)
	}

	// Sender report for the video SSRC; its NTP middle 32 feeds the good RR.
	srTime := b.cursor
	srMsg := sr(SSRCVideo, srTime)
	srMiddle := (srMsg.SenderInfo.NTPMSW << 16) | (srMsg.SenderInfo.NTPLSW >> 16)
	b.rtcp(rtproto.DirSend, srMsg)
	b.advance(10_000)

	// Two TWCC messages with overlapping transport sequence windows. The
	// advances between the SR and the good RR below sum to exactly 50 ms, so
	// the good RR lands one clean RTT after the SR with a monotonic clock.
	b.rtcp(rtproto.DirRecv, twcc(100, 11, b.cursor))
	b.advance(10_000)
	b.rtcp(rtproto.DirRecv, twcc(105, 9, b.cursor)) // 105..113, overlaps 105..110
	b.advance(15_000)

	// NACK for the lost seqs; resolves inside the first generation.
	b.rtcp(rtproto.DirRecv, nack(SSRCVideo, rtproto.NACKRange{Start: 4, End: 5}))
	b.advance(15_000)

	// Good receiver report: LSR points at the SR above, DLSR=0 -> RTT 50 ms.
	goodRRTime := b.cursor
	if goodRRTime != srTime+50_000 {
		panic("sample timing drift: good RR must sit exactly 50 ms after the SR")
	}
	b.rtcp(rtproto.DirRecv, rr(block(SSRCVideo, srMiddle, 0)))
	b.advance(20_000)

	// --- Phase 2: simulcast switch down to the low layer -------------------
	for seq := 1000; seq <= 1003; seq++ {
		b.rtp(SSRCLow, seq, "0", "l", 120+(seq-1000), false)
		b.advance(20_000)
	}
	// 400 ms encoder silence with continuous sequence numbers.
	b.advance(400_000)
	b.rtp(SSRCLow, 1004, "0", "l", 124, true)
	b.advance(20_000)

	// Receiver report for the low layer, which never sent an SR: LSR=0.
	b.rtcp(rtproto.DirRecv, rr(block(SSRCLow, 0, 0)))
	b.advance(20_000)

	// Switch back to the high layer; same payload type, fresh generation.
	for seq := 10; seq <= 13; seq++ {
		b.rtp(SSRCVideo, seq, "0", "h", 130+(seq-10), false)
		b.advance(20_000)
	}

	// --- Phase 3: BYE, rebirth, and a NACK from a stale SSRC generation ----
	b.rtcp(rtproto.DirSend, bye(SSRCVideo))
	b.advance(100_000)
	for seq := 20; seq <= 23; seq++ {
		b.rtp(SSRCVideo, seq, "0", "h", 140+(seq-20), false)
		b.advance(20_000)
	}
	// Late NACK for a seq only the first generation ever carried. The SSRC
	// has been reused, so every generation that held it stays a candidate.
	b.rtcp(rtproto.DirRecv, nack(SSRCVideo, rtproto.NACKRange{Start: 65532, End: 65532}))
	b.advance(20_000)
	// And one for a seq the reborn generation did carry.
	b.rtcp(rtproto.DirRecv, nack(SSRCVideo, rtproto.NACKRange{Start: 21, End: 21}))
	b.advance(20_000)

	// --- Phase 4: keyframe-request storm: four PLIs plus an FIR in ~75 ms ---
	for i := 0; i < 4; i++ {
		b.rtcp(rtproto.DirRecv, pli(SSRCVideo))
		b.advance(15_000)
	}
	b.rtcp(rtproto.DirRecv, fir(SSRCVideo, 1))
	b.advance(15_000)

	// --- Phase 5: ICE restart, transport sequence resets -------------------
	b.add(rtproto.Packet{
		TimeUS: b.cursor, Direction: rtproto.DirSend, Kind: rtproto.KindRTCP,
		RTCP: []rtproto.RTCPMessage{{
			PacketType: rtproto.PTAPP, SSRC: SSRCVideo, Reason: "ice_restart",
		}},
	})
	b.advance(50_000)
	// Same transport seq numbers as epoch 0, but a new epoch namespace.
	for seq := 500; seq <= 505; seq++ {
		b.rtp(SSRCVideo, seq, "0", "h", 100+(seq-500), false)
		b.advance(20_000)
	}
	b.rtcp(rtproto.DirRecv, twcc(100, 6, b.cursor)) // resolves only in epoch 1
	b.advance(20_000)
	// NACK arriving after the restart for a pre-restart sequence.
	b.rtcp(rtproto.DirRecv, nack(SSRCVideo, rtproto.NACKRange{Start: 65534, End: 65534}))
	b.advance(20_000)

	// --- Phase 6: SSRC collision on a second media line --------------------
	for seq := 1; seq <= 3; seq++ {
		b.rtp(SSRCOther, seq, "1", "", 200+(seq-1), false)
		b.advance(20_000)
	}
	// The video SSRC value reappears on mid=1: collision, hard boundary.
	b.rtp(SSRCVideo, 700, "1", "x", 210, true)
	b.advance(20_000)

	// RR whose LSR matches no sender report; the second line has no SR.
	b.rtcp(rtproto.DirRecv, rr(block(SSRCOther, 0x12340000, 1000)))

	return b.packets
}

// JSONL renders the capture as JSON Lines text.
func JSONL() ([]byte, error) {
	packets := Build()
	out := make([]byte, 0, len(packets)*256)
	for _, p := range packets {
		line, err := json.Marshal(p)
		if err != nil {
			return nil, err
		}
		out = append(out, line...)
		out = append(out, '\n')
	}
	return out, nil
}
