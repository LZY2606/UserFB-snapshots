package analyze

import (
	"testing"

	"rtpfreeze/internal/identity"
	"rtpfreeze/internal/rtproto"
)

func tseq(v uint16) *uint16 { return &v }

func rtp(idx int, ssrc uint32, seq uint16, mid, rid string, transport *uint16) rtproto.Packet {
	return rtproto.Packet{
		Index: idx, TimeUS: int64(idx) * 20000, Direction: rtproto.DirSend,
		Kind: rtproto.KindRTP, TransportSeq: transport,
		RTP: &rtproto.RTPHeader{Version: 2, PayloadType: 96, Sequence: seq, SSRC: ssrc},
		MID: mid, RID: rid,
	}
}

func rtcp(idx int, dir rtproto.Direction, msgs ...rtproto.RTCPMessage) rtproto.Packet {
	return rtproto.Packet{
		Index: idx, TimeUS: int64(idx) * 20000, Direction: dir,
		Kind: rtproto.KindRTCP, RTCP: msgs,
	}
}

func runEngine(t *testing.T, packets []rtproto.Packet, events []identity.Event) (*identity.Result, *Analysis) {
	t.Helper()
	id := identity.Build(packets, events)
	eng := NewEngine(packets, id, DefaultFreezeParams())
	return id, eng.Run()
}

func findFeedback(res *Analysis, kind string, nth int) *FeedbackEvent {
	found := 0
	for i := range res.Feedback {
		if res.Feedback[i].Kind == kind {
			if found == nth {
				return &res.Feedback[i]
			}
			found++
		}
	}
	return nil
}

func TestNACKKeepsAllGenerationCandidates(t *testing.T) {
	// Generation 0 carries seq 10; BYE; the SSRC is reborn in gen 1; a NACK
	// for seq 10 then has candidates in BOTH generations and stays ambiguous.
	packets := []rtproto.Packet{
		rtp(0, 0x42, 9, "0", "h", tseq(1)),
		rtp(1, 0x42, 10, "0", "h", tseq(2)),
		rtcp(2, rtproto.DirSend, rtproto.RTCPMessage{PacketType: rtproto.PTBYE, SSRC: 0x42}),
		rtp(3, 0x42, 1, "0", "h", tseq(3)),
		rtp(4, 0x42, 2, "0", "h", tseq(4)),
		rtcp(5, rtproto.DirRecv, rtproto.RTCPMessage{
			FMT: rtproto.FMTNACK, PacketType: rtproto.PTFB, SSRC: 0x99, MediaSSRC: 0x42,
			NACK: []rtproto.NACKRange{{Start: 10, End: 10}},
		}),
	}
	_, res := runEngine(t, packets, nil)
	ev := findFeedback(res, KindNACK, 0)
	if ev == nil {
		t.Fatal("no NACK event analyzed")
	}
	if ev.Status != StatusAmbiguous {
		t.Fatalf("reused-SSRC NACK status = %s, want ambiguous", ev.Status)
	}
	var gens []int
	resolvedPackets := 0
	for _, c := range ev.Candidates {
		gens = append(gens, c.GenID)
		if c.PacketIndex >= 0 {
			resolvedPackets++
		}
	}
	if len(unique(gens)) != 2 {
		t.Fatalf("expected candidates on 2 generations, got %v", gens)
	}
	// Gen 0 observed seq 10 (packet 1); gen 1 never did, so exactly one
	// candidate resolves to a sent packet and the other remains open.
	if resolvedPackets != 1 {
		t.Fatalf("want exactly 1 resolved send event, got %d", resolvedPackets)
	}
}

func TestTWCCOverlappingWindowsAndEpochReset(t *testing.T) {
	packets := []rtproto.Packet{
		rtp(0, 0x42, 0, "0", "h", tseq(100)),
		rtp(1, 0x42, 1, "0", "h", tseq(101)),
		rtp(2, 0x42, 2, "0", "h", tseq(102)),
		// Two overlapping feedback windows 100..102 and 101..102.
		rtcp(3, rtproto.DirRecv, rtproto.RTCPMessage{
			FMT: rtproto.FMTTWCC, PacketType: rtproto.PTFB, SSRC: 0x99,
			TWCC: &rtproto.TWCCFeedback{BaseSequence: 100, Runs: []rtproto.TWCCRun{
				{FirstTransportSeq: 100, Count: 3},
			}},
		}),
		rtcp(4, rtproto.DirRecv, rtproto.RTCPMessage{
			FMT: rtproto.FMTTWCC, PacketType: rtproto.PTFB, SSRC: 0x99,
			TWCC: &rtproto.TWCCFeedback{BaseSequence: 101, Runs: []rtproto.TWCCRun{
				{FirstTransportSeq: 101, Count: 2},
			}},
		}),
		// ICE restart resets the transport sequence namespace.
		rtcp(5, rtproto.DirSend, rtproto.RTCPMessage{
			PacketType: rtproto.PTAPP, SSRC: 0x42, Reason: "ice_restart",
		}),
		rtp(6, 0x42, 500, "0", "h", tseq(100)), // same tseq 100, new epoch
		rtcp(7, rtproto.DirRecv, rtproto.RTCPMessage{
			FMT: rtproto.FMTTWCC, PacketType: rtproto.PTFB, SSRC: 0x99,
			TWCC: &rtproto.TWCCFeedback{BaseSequence: 100, Runs: []rtproto.TWCCRun{
				{FirstTransportSeq: 100, Count: 1},
			}},
		}),
	}
	id, res := runEngine(t, packets, []identity.Event{{Index: 5, TimeUS: 100000, Type: "ice_restart"}})

	first := findFeedback(res, KindTWCC, 0)
	if first == nil || first.Status != StatusResolved {
		t.Fatalf("first TWCC = %+v, want resolved", first)
	}
	second := findFeedback(res, KindTWCC, 1)
	if len(second.Candidates) != 2 {
		t.Fatalf("overlapping window should retain 2 candidates, got %d", len(second.Candidates))
	}
	// Both overlapping messages must point at the SAME sent packets, never at
	// packets from the other epoch.
	for _, c := range second.Candidates {
		if c.TransportEpochID != 0 {
			t.Fatalf("epoch-0 feedback resolved into epoch %d", c.TransportEpochID)
		}
	}

	third := findFeedback(res, KindTWCC, 2)
	if third.TransportEpochID != 1 {
		t.Fatalf("post-restart feedback epoch = %d, want 1", third.TransportEpochID)
	}
	if len(third.Candidates) != 1 || third.Candidates[0].PacketIndex != 6 {
		t.Fatalf("post-restart TWCC must map only to the epoch-1 packet: %+v", third.Candidates)
	}
	if id.TransportEpochs[1].Reason != identity.BoundaryICERestart {
		t.Fatal("second epoch not labeled as ICE restart")
	}
}

func TestRTTCandidates(t *testing.T) {
	// SR at packet 1, t=20ms. RR at packet 3, t=60ms with LSR=middle32(SR),
	// DLSR=0 -> RTT must be 40ms. A second block with LSR=0 cannot be timed.
	srUS := int64(20_000)
	sr := rtproto.RTCPMessage{PacketType: rtproto.PTSR, SSRC: 0x42,
		SenderInfo: &rtproto.SenderInfo{NTPMSW: uint32(srUS / 1_000_000),
			NTPLSW:       uint32(float64(srUS%1_000_000) / 1e6 * (1 << 32)),
			RTPTimestamp: 1000}}
	middle := (sr.SenderInfo.NTPMSW << 16) | (sr.SenderInfo.NTPLSW >> 16)

	packets := []rtproto.Packet{
		rtp(0, 0x42, 0, "0", "h", tseq(1)),
		rtcp(1, rtproto.DirSend, sr),
		rtp(2, 0x42, 1, "0", "h", tseq(2)),
		rtcp(3, rtproto.DirRecv, rtproto.RTCPMessage{
			PacketType: rtproto.PTRR, SSRC: 0x99,
			ReportBlocks: []rtproto.ReportBlock{
				{SSRC: 0x42, LSR: middle, DLSR: 0},
				{SSRC: 0x77, LSR: 0, DLSR: 0}, // never received an SR
			},
		}),
	}
	_, res := runEngine(t, packets, nil)
	ev := findFeedback(res, KindRR, 0)
	if ev == nil {
		t.Fatal("no receiver report analyzed")
	}
	if len(ev.RTTUS) != 2 {
		t.Fatalf("want 2 RTT slots, got %d", len(ev.RTTUS))
	}
	// Packet 3 timestamp is 60000us; SR NTP 20000us; DLSR 0 -> 40000us.
	if got := ev.RTTUS[0]; got != 40_000 {
		t.Fatalf("RTT candidate = %d us, want 40000", got)
	}
	if ev.RTTUS[1] != 0 || ev.RTTNotes[1] == "" {
		t.Fatalf("LSR=0 block must stay untimed: %+v", ev.RTTNotes)
	}

	// Unmatched LSR marks the event unresolved rather than inventing an RTT.
	packets = append(packets, rtcp(4, rtproto.DirRecv, rtproto.RTCPMessage{
		PacketType: rtproto.PTRR, SSRC: 0x99,
		ReportBlocks: []rtproto.ReportBlock{{SSRC: 0x42, LSR: 0xDEADBEEF, DLSR: 100}},
	}))
	_, res = runEngine(t, packets, nil)
	if len(res.Feedback) < 2 {
		t.Fatalf("want 2 RR events, got %d", len(res.Feedback))
	}
	bad := res.Feedback[len(res.Feedback)-1]
	if bad.Status != StatusUnresolved {
		t.Fatalf("unmatched LSR status = %s, want unresolved", bad.Status)
	}
	if bad.RTTUS[0] != 0 {
		t.Fatalf("unmatched LSR must not fabricate an RTT, got %d", bad.RTTUS[0])
	}
}

func TestFreezeDetectionAndFeedbackLink(t *testing.T) {
	// Seqs 0,1 then a jump to 5 (missing 2,3,4) then silence, then recovery.
	packets := []rtproto.Packet{
		rtp(0, 0x42, 0, "0", "h", tseq(1)),
		rtp(1, 0x42, 1, "0", "h", tseq(2)),
		rtcp(2, rtproto.DirRecv, rtproto.RTCPMessage{
			FMT: rtproto.FMTNACK, PacketType: rtproto.PTFB, SSRC: 0x99, MediaSSRC: 0x42,
			NACK: []rtproto.NACKRange{{Start: 2, End: 4}},
		}),
		rtp(3, 0x42, 5, "0", "h", tseq(3)),
	}
	packets[3].TimeUS = packets[1].TimeUS + 400_000
	_, res := runEngine(t, packets, nil)

	var lossFreeze *FreezeInterval
	for i := range res.Freezes {
		if res.Freezes[i].Kind == "loss_gap" {
			lossFreeze = &res.Freezes[i]
		}
	}
	if lossFreeze == nil {
		t.Fatalf("no loss_gap freeze detected: %+v", res.Freezes)
	}
	if lossFreeze.MissingCount != 3 || lossFreeze.FirstMissingExt != 2 || lossFreeze.LastMissingExt != 4 {
		t.Fatalf("freeze extent wrong: %+v", lossFreeze)
	}
	if len(lossFreeze.RelatedFeedback) == 0 {
		t.Fatal("loss freeze not linked to the NACK that reports the same generation")
	}
}

func TestFeedbackNeverCrossesGenerationBoundary(t *testing.T) {
	// A NACK for an SSRC that exists in two generations must not resolve a
	// sequence onto a packet of the wrong generation.
	packets := []rtproto.Packet{
		rtp(0, 0x42, 7, "0", "h", tseq(1)),
		rtcp(1, rtproto.DirSend, rtproto.RTCPMessage{PacketType: rtproto.PTBYE, SSRC: 0x42}),
		rtp(2, 0x42, 7, "0", "h", tseq(2)),
		rtcp(3, rtproto.DirRecv, rtproto.RTCPMessage{
			FMT: rtproto.FMTNACK, PacketType: rtproto.PTFB, SSRC: 0x99, MediaSSRC: 0x42,
			NACK: []rtproto.NACKRange{{Start: 7, End: 7}},
		}),
	}
	_, res := runEngine(t, packets, nil)
	ev := findFeedback(res, KindNACK, 0)
	if len(ev.Candidates) != 2 {
		t.Fatalf("want one candidate per generation, got %d", len(ev.Candidates))
	}
	packetIdx := map[int]bool{}
	for _, c := range ev.Candidates {
		if c.PacketIndex >= 0 {
			packetIdx[c.PacketIndex] = true
		}
	}
	if !packetIdx[0] || !packetIdx[2] {
		t.Fatalf("each generation's seq-7 packet must be its own candidate: %v", packetIdx)
	}
}

func unique(in []int) []int {
	seen := map[int]bool{}
	var out []int
	for _, v := range in {
		if !seen[v] {
			seen[v] = true
			out = append(out, v)
		}
	}
	return out
}

func TestFIRAndPLIAreRoutedWithCandidates(t *testing.T) {
	packets := []rtproto.Packet{
		rtp(0, 0x42, 1, "0", "h", tseq(1)),
		rtcp(1, rtproto.DirRecv,
			rtproto.RTCPMessage{
				FMT: rtproto.FMTPLI, PacketType: rtproto.PTPSFB, SSRC: 0x99, MediaSSRC: 0x42,
			},
			rtproto.RTCPMessage{
				FMT: rtproto.FMTFIR, PacketType: rtproto.PTPSFB, SSRC: 0x99, MediaSSRC: 0x42, FIRSeq: 7,
			}),
		// A PLI for an SSRC that never existed cannot be resolved.
		rtcp(2, rtproto.DirRecv, rtproto.RTCPMessage{
			FMT: rtproto.FMTFIR, PacketType: rtproto.PTPSFB, SSRC: 0x99, MediaSSRC: 0x77,
		}),
	}
	_, res := runEngine(t, packets, nil)

	pli := findFeedback(res, KindPLI, 0)
	if pli == nil || pli.Status != StatusResolved || len(pli.Candidates) != 1 {
		t.Fatalf("PLI routing wrong: %+v", pli)
	}
	fir := findFeedback(res, KindFIR, 0)
	if fir == nil || fir.Status != StatusResolved {
		t.Fatalf("FIR routing wrong: %+v", fir)
	}
	if fir.Candidates[0].GenID != 0 {
		t.Fatalf("FIR candidate must point at the live generation: %+v", fir.Candidates)
	}
	ghost := findFeedback(res, KindFIR, 1)
	if ghost == nil || ghost.Status != StatusUnresolved {
		t.Fatalf("unknown-SSRC FIR must be unresolved: %+v", ghost)
	}
}
