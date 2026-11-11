package capture

import (
	"bytes"
	"testing"

	"rtpfreeze/internal/rtproto"
)

func collect(packets []rtproto.Packet) (nack, pli, fir, twcc, rr, sr, bye, app int, maxSeq, minSeq uint16, sawWrap bool) {
	prev := -1
	for _, p := range packets {
		if p.Kind == rtproto.KindRTP {
			seq := int(p.RTP.Sequence)
			if prev >= 0 && seq == 0 && prev > 60000 {
				sawWrap = true
			}
			if seq > int(maxSeq) {
				maxSeq = p.RTP.Sequence
			}
			if minSeq == 0 || p.RTP.Sequence < minSeq {
				minSeq = p.RTP.Sequence
			}
			prev = seq
		}
		for _, m := range p.RTCP {
			switch {
			case m.PacketType == rtproto.PTFB && m.FMT == rtproto.FMTNACK:
				nack++
			case m.PacketType == rtproto.PTPSFB && m.FMT == rtproto.FMTPLI:
				pli++
			case m.PacketType == rtproto.PTPSFB && m.FMT == rtproto.FMTFIR:
				fir++
			case m.PacketType == rtproto.PTFB && m.FMT == rtproto.FMTTWCC:
				twcc++
			case m.PacketType == rtproto.PTRR:
				rr++
			case m.PacketType == rtproto.PTSR:
				sr++
			case m.PacketType == rtproto.PTBYE:
				bye++
			case m.PacketType == rtproto.PTAPP && m.Reason == "ice_restart":
				app++
			}
		}
	}
	return
}

func TestSampleContainsRequiredScenarios(t *testing.T) {
	packets := Build()

	// Every packet keeps its original, unique index in capture order.
	for i, p := range packets {
		if p.Index != i {
			t.Fatalf("packet %d has index %d", i, p.Index)
		}
	}

	nack, pli, fir, twcc, rr, sr, bye, app, maxSeq, minSeq, sawWrap := collect(packets)
	if !sawWrap {
		t.Fatal("sample must contain an RTP sequence wrap past 65535")
	}
	if maxSeq < 65530 || minSeq > 9 {
		t.Fatalf("sequence range %d..%d does not straddle the wrap", minSeq, maxSeq)
	}
	if twcc < 2 {
		t.Fatalf("want overlapping TWCC messages, got %d", twcc)
	}
	if nack < 3 {
		t.Fatalf("want current, stale-SSRC and post-restart NACKs, got %d", nack)
	}
	if pli < 4 {
		t.Fatalf("want a PLI storm (>=4), got %d", pli)
	}
	if fir < 1 {
		t.Fatalf("want an FIR alongside the keyframe-request storm, got %d", fir)
	}
	if sr == 0 {
		t.Fatal("sample needs at least one sender report for the good-RR path")
	}
	// Receiver reports cover three cases: matched LSR, LSR=0, unmatched LSR.
	if rr < 3 {
		t.Fatalf("want >=3 receiver reports, got %d", rr)
	}
	if bye != 1 || app != 1 {
		t.Fatalf("want one BYE and one ICE restart marker, got bye=%d app=%d", bye, app)
	}

	// The low layer and second media line have no SR, so sender reports must
	// not cover every SSRC.
	srSSRC := map[uint32]bool{}
	for _, p := range packets {
		for _, m := range p.RTCP {
			if m.PacketType == rtproto.PTSR {
				srSSRC[m.SSRC] = true
			}
		}
	}
	if srSSRC[SSRCLow] || srSSRC[SSRCOther] {
		t.Fatal("low layer and second line must have missing sender reports")
	}
}

func TestJSONLRoundTrip(t *testing.T) {
	raw, err := JSONL()
	if err != nil {
		t.Fatal(err)
	}
	got, err := rtproto.DecodeCapture(bytes.NewReader(raw))
	if err != nil {
		t.Fatal(err)
	}
	want := Build()
	if len(got) != len(want) {
		t.Fatalf("decoded %d packets, want %d", len(got), len(want))
	}
	for i := range want {
		if got[i].Index != want[i].Index || got[i].Kind != want[i].Kind {
			t.Fatalf("packet %d mismatch after JSONL round trip", i)
		}
	}
}
