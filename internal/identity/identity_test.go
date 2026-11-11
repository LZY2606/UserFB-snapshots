package identity

import (
	"testing"

	"rtpfreeze/internal/rtproto"
)

func rtpPacket(idx int, ssrc uint32, seq uint16, mid, rid string) rtproto.Packet {
	return rtproto.Packet{
		Index: idx, TimeUS: int64(idx) * 20000, Direction: rtproto.DirSend,
		Kind: rtproto.KindRTP,
		RTP:  &rtproto.RTPHeader{Version: 2, PayloadType: 96, Sequence: seq, SSRC: ssrc},
		MID:  mid, RID: rid,
	}
}

func rtcpPacket(idx int, m rtproto.RTCPMessage) rtproto.Packet {
	return rtproto.Packet{
		Index: idx, TimeUS: int64(idx) * 20000, Direction: rtproto.DirRecv,
		Kind: rtproto.KindRTCP, RTCP: []rtproto.RTCPMessage{m},
	}
}

func genSSRCSet(r *Result) map[uint32][]int {
	out := map[uint32][]int{}
	for i, g := range r.Generations {
		out[g.SSRC] = append(out[g.SSRC], i)
	}
	return out
}

func TestSimulcastLayerSwitchReusesPayloadType(t *testing.T) {
	// Same MID, RID h -> l -> h, different SSRCs, identical payload type 96.
	packets := []rtproto.Packet{
		rtpPacket(0, 0xA, 1, "0", "h"),
		rtpPacket(1, 0xA, 2, "0", "h"),
		rtpPacket(2, 0xB, 1, "0", "l"),
		rtpPacket(3, 0xB, 2, "0", "l"),
		rtpPacket(4, 0xA, 3, "0", "h"),
		rtpPacket(5, 0xA, 4, "0", "h"),
	}
	r := Build(packets, nil)

	if len(r.Tracks) != 2 {
		t.Fatalf("want 2 tracks (h/l), got %d: %+v", len(r.Tracks), r.Tracks)
	}
	for _, tr := range r.Tracks {
		if tr.PayloadType != 96 {
			t.Fatalf("payload type changed across layers: %+v", tr)
		}
	}
	// Three generations: high, low (closes high via layer switch), high again.
	if len(r.Generations) != 3 {
		t.Fatalf("want 3 generations, got %d", len(r.Generations))
	}
	gens := r.Generations
	if gens[0].CloseReason != CloseLayerSwitch || gens[1].OpenReason != BoundaryLayerSwitch {
		t.Fatalf("layer switch boundary not recorded: %+v", gens)
	}
	if gens[1].TrackID == gens[0].TrackID {
		t.Fatal("high and low layer mapped to the same track")
	}
	if gens[2].OpenReason != BoundaryLayerSwitch {
		t.Fatalf("resuming high layer should be a layer switch, got %s", gens[2].OpenReason)
	}
	// Sequence continuity is per generation: the resumed high layer extends
	// 1,2 -> 3,4 rather than restarting at the low layer's sequence.
	if r.RTP[4].ExtendedSeq != 3 || r.RTP[5].ExtendedSeq != 4 {
		t.Fatalf("high-layer extended seq broken by switch: %d %d",
			r.RTP[4].ExtendedSeq, r.RTP[5].ExtendedSeq)
	}
	if r.RTP[2].ExtendedSeq != 1 || r.RTP[3].ExtendedSeq != 2 {
		t.Fatalf("low-layer extended seq should restart at 1: %d", r.RTP[2].ExtendedSeq)
	}
}

func TestSSRCCollisionIsHardBoundary(t *testing.T) {
	// SSRC 0x7 feeds mid=0, then the same value appears on mid=1.
	packets := []rtproto.Packet{
		rtpPacket(0, 0x7, 1, "0", "h"),
		rtpPacket(1, 0x7, 2, "0", "h"),
		rtpPacket(2, 0x7, 1, "1", "h"),
		rtpPacket(3, 0x7, 2, "1", "h"),
	}
	r := Build(packets, nil)
	if len(r.Generations) != 2 {
		t.Fatalf("want 2 generations across collision, got %d", len(r.Generations))
	}
	g0, g1 := r.Generations[0], r.Generations[1]
	if g0.CloseReason != CloseCollision || g1.OpenReason != BoundaryCollision {
		t.Fatalf("collision boundary wrong: %s / %s", g0.CloseReason, g1.OpenReason)
	}
	if g0.TrackID == g1.TrackID {
		t.Fatal("collision generations must belong to different tracks")
	}
	// Both sides retain the same raw SSRC but sequence spaces are independent.
	if r.GenerationsForSSRC(0x7) == nil || len(r.GenerationsForSSRC(0x7)) != 2 {
		t.Fatal("both collision generations must be discoverable by SSRC")
	}
	if r.RTP[2].ExtendedSeq != 1 {
		t.Fatalf("post-collision seq must restart, got %d", r.RTP[2].ExtendedSeq)
	}
}

func TestBYERebirthIsNewGeneration(t *testing.T) {
	packets := []rtproto.Packet{
		rtpPacket(0, 0x9, 10, "0", "h"),
		rtpPacket(1, 0x9, 11, "0", "h"),
		rtcpPacket(2, rtproto.RTCPMessage{PacketType: rtproto.PTBYE, SSRC: 0x9}),
		rtpPacket(3, 0x9, 1, "0", "h"),
		rtpPacket(4, 0x9, 2, "0", "h"),
	}
	r := Build(packets, nil)
	if len(r.Generations) != 2 {
		t.Fatalf("want 2 generations around BYE, got %d", len(r.Generations))
	}
	if r.Generations[0].CloseReason != CloseBYE {
		t.Fatalf("first gen close = %s, want bye", r.Generations[0].CloseReason)
	}
	if r.Generations[1].OpenReason != BoundaryBYERebirth {
		t.Fatalf("rebirth open reason = %s, want bye_rebirth", r.Generations[1].OpenReason)
	}
	if r.RTP[3].ExtendedSeq != 1 {
		t.Fatalf("reborn SSRC must restart sequence space, got %d", r.RTP[3].ExtendedSeq)
	}
	if r.Generations[1].GenerationNo != 2 {
		t.Fatalf("SSRC reuse counter = %d, want 2", r.Generations[1].GenerationNo)
	}
}

func TestICERestartResetsEpochsAndGenerations(t *testing.T) {
	packets := []rtproto.Packet{
		rtpPacket(0, 0x3, 500, "0", "h"),
		rtpPacket(1, 0x3, 501, "0", "h"),
		rtcpPacket(2, rtproto.RTCPMessage{PacketType: rtproto.PTAPP, SSRC: 0x3, Reason: "ice_restart"}),
		rtpPacket(3, 0x3, 1, "0", "h"),
		rtpPacket(4, 0x3, 2, "0", "h"),
	}
	r := Build(packets, []Event{{Index: 2, TimeUS: 40000, Type: "ice_restart"}})
	if len(r.TransportEpochs) != 2 {
		t.Fatalf("want 2 transport epochs, got %d", len(r.TransportEpochs))
	}
	if r.TransportEpochs[0].EndIndex != 2 || r.TransportEpochs[1].StartIndex != 2 {
		t.Fatalf("epoch split not at ICE restart index: %+v", r.TransportEpochs)
	}
	if len(r.Generations) != 2 {
		t.Fatalf("want 2 generations across ICE restart, got %d", len(r.Generations))
	}
	g1 := r.Generations[1]
	if g1.OpenReason != BoundaryICERestart {
		t.Fatalf("post-restart gen reason = %s", g1.OpenReason)
	}
	if r.RTP[1].TransportEpochID == r.RTP[3].TransportEpochID {
		t.Fatal("packets on either side of an ICE restart share a transport epoch")
	}
	if r.RTP[3].ExtendedSeq != 1 {
		t.Fatalf("sequence space should reset after ICE restart, got %d", r.RTP[3].ExtendedSeq)
	}
}

func TestGenerationsForSSRCFindsEveryReuse(t *testing.T) {
	packets := []rtproto.Packet{
		rtpPacket(0, 0x5, 1, "0", "h"),
		rtcpPacket(1, rtproto.RTCPMessage{PacketType: rtproto.PTBYE, SSRC: 0x5}),
		rtpPacket(2, 0x5, 1, "0", "h"),
		rtcpPacket(3, rtproto.RTCPMessage{PacketType: rtproto.PTAPP, SSRC: 0x5, Reason: "ice_restart"}),
		rtpPacket(4, 0x5, 1, "0", "h"),
	}
	r := Build(packets, []Event{{Index: 3, TimeUS: 60000, Type: "ice_restart"}})
	gens := r.GenerationsForSSRC(0x5)
	if len(gens) != 3 {
		t.Fatalf("want 3 generations reusing SSRC, got %d", len(gens))
	}
}
