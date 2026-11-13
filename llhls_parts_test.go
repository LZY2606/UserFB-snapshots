/*
Package m3u8. Low-Latency HLS partial segment tests.

Copyright 2024 The Project Developers.
See the AUTHORS and LICENSE files at the top-level directory of this distribution
and at https://github.com/grafov/m3u8/
*/
package m3u8

import (
	"fmt"
	"strings"
	"testing"
)

// indexOf returns the position of s in out or -1 when it is absent.
func indexOf(out, s string) int {
	return strings.Index(out, s)
}

// TestLLHLSPartLifecycle follows parts from publishing over a failed
// commit to committing them as a complete media segment.
func TestLLHLSPartLifecycle(t *testing.T) {
	p, err := NewMediaPlaylist(4, 4)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err = p.SetPartTargetDuration(0.5); err != nil {
		t.Fatalf("SetPartTargetDuration failed: %s", err)
	}

	first := &Part{URI: "78.0.m4s", Duration: 0.5, Independent: true}
	second := &Part{URI: "78.1.m4s", Duration: 0.5}
	if err = p.AppendPart(first); err != nil {
		t.Fatalf("AppendPart #1 failed: %s", err)
	}
	if err = p.AppendPart(second); err != nil {
		t.Fatalf("AppendPart #2 failed: %s", err)
	}
	hint := &PreloadHint{URI: "78.2.m4s"}
	if err = p.SetPreloadHint(hint); err != nil {
		t.Fatalf("SetPreloadHint failed: %s", err)
	}
	p.SetTailKey(&Key{Method: "AES-128", URI: "key.bin"})
	p.SetTailDiscontinuity()

	// Parts and the hint are not complete segments.
	if p.Count() != 0 {
		t.Errorf("Count() counts tail parts: got %d, want 0", p.Count())
	}
	if parts := p.TailParts(); len(parts) != 2 || parts[0].URI != "78.0.m4s" || !parts[0].Independent {
		t.Errorf("TailParts() mismatch: %+v", parts)
	}
	if got := p.PreloadHint(); got == nil || got.URI != "78.2.m4s" {
		t.Errorf("PreloadHint() mismatch: %+v", got)
	}
	// The tail belongs to SeqNo + number of complete segments.
	if p.tailSeqNo() != p.SeqNo {
		t.Errorf("tail sequence: got %d, want %d", p.tailSeqNo(), p.SeqNo)
	}

	out := p.Encode().String()
	for _, tag := range []string{
		"#EXT-X-PART-INF:PART-TARGET=0.5\n",
		"#EXT-X-PART:DURATION=0.5,INDEPENDENT=YES,URI=\"78.0.m4s\"",
		"#EXT-X-PART:DURATION=0.5,URI=\"78.1.m4s\"",
		"#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"78.2.m4s\"",
	} {
		if !strings.Contains(out, tag) {
			t.Errorf("encoded playlist misses %q:\n%s", tag, out)
		}
	}
	if strings.Contains(out, "#EXTINF") {
		t.Errorf("tail parts must be emitted without EXTINF:\n%s", out)
	}
	if iKey, iPart, iHint := indexOf(out, "#EXT-X-KEY"), indexOf(out, "#EXT-X-PART:"), indexOf(out, "#EXT-X-PRELOAD-HINT"); !(iKey >= 0 && iKey < iPart && iPart < iHint) {
		t.Errorf("tail tag order wrong: key=%d part=%d hint=%d", iKey, iPart, iHint)
	}
	if strings.Index(out, "#EXT-X-DISCONTINUITY") > indexOf(out, "#EXT-X-PART:") {
		t.Errorf("discontinuity must be declared before the parts:\n%s", out)
	}

	// The playlist keeps copies: mutating the caller's values must not
	// change the next Encode output.
	first.URI = "mutated.m4s"
	first.Duration = 42
	hint.URI = "mutated-hint.m4s"
	if again := p.Encode().String(); again != out {
		t.Errorf("Encode output changed after caller mutation:\nbefore:\n%s\nafter:\n%s", out, again)
	}
	if strings.Contains(out, "mutated") {
		t.Errorf("caller mutation leaked into playlist: %s", out)
	}

	// Invalid updates must not change the published content.
	bad := &Part{URI: "bad.m4s", Duration: 0}
	if err = p.AppendPart(bad); err != ErrPartDuration {
		t.Errorf("zero duration: got %v, want ErrPartDuration", err)
	}
	if err = p.AppendPart(&Part{URI: "", Duration: 0.5}); err != ErrPartURI {
		t.Errorf("empty URI: got %v, want ErrPartURI", err)
	}
	if again := p.Encode().String(); again != out {
		t.Errorf("failed AppendPart changed the playlist:\n%s", again)
	}

	// Fill the complete-segment capacity to force a failed commit.
	for i := 0; i < 4; i++ {
		if err = p.Append(fmt.Sprintf("full%d.ts", i), 2.0, ""); err != nil {
			t.Fatalf("fill segment %d failed: %s", i, err)
		}
	}
	if err = p.CommitSegment("78.m4s", 2.0, ""); err != ErrPlaylistFull {
		t.Errorf("full playlist commit: got %v, want ErrPlaylistFull", err)
	}
	if p.Count() != 4 || len(p.TailParts()) != 2 || p.PreloadHint() == nil {
		t.Errorf("failed commit lost published tail: count=%d parts=%d hint=%v", p.Count(), len(p.TailParts()), p.PreloadHint())
	}
	failed := p.Encode().String()
	if !strings.Contains(failed, "78.0.m4s") || !strings.Contains(failed, "#EXT-X-PRELOAD-HINT") {
		t.Errorf("failed commit dropped published tags:\n%s", failed)
	}

	// Slide one complete segment out, then commit the tail successfully.
	if err = p.Remove(); err != nil {
		t.Fatalf("Remove failed: %s", err)
	}
	if err = p.CommitSegment("78.m4s", 2.0, ""); err != nil {
		t.Fatalf("CommitSegment failed: %s", err)
	}
	if p.Count() != 4 {
		t.Errorf("Count after commit: got %d, want 4", p.Count())
	}
	committed := p.Segments[p.last()]
	if committed.SeqId != 4 || committed.URI != "78.m4s" || len(committed.Parts) != 2 {
		t.Errorf("committed segment mismatch: %+v", committed)
	}
	if committed.Key == nil || committed.Key.URI != "key.bin" || !committed.Discontinuity {
		t.Errorf("tail declarations did not move with the segment: %+v", committed)
	}
	if p.TailParts() != nil || p.PreloadHint() != nil {
		t.Errorf("commit must clear the tail parts and the preload hint")
	}
	encoded := p.Encode().String()
	iPart := indexOf(encoded, "#EXT-X-PART:DURATION=0.5,INDEPENDENT=YES")
	iInf := indexOf(encoded, "#EXTINF:2.000,\n78.m4s")
	iKey := indexOf(encoded, "#EXT-X-KEY:METHOD=AES-128")
	if !(iKey >= 0 && iKey < iPart && iPart >= 0 && iPart < iInf) {
		t.Errorf("committed segment order wrong: key=%d part=%d extinf=%d", iKey, iPart, iInf)
	}

	// The next part belongs to the next sequence number.
	if err = p.Remove(); err != nil {
		t.Fatalf("Remove #2 failed: %s", err)
	}
	if err = p.AppendPart(&Part{URI: "79.0.m4s", Duration: 0.5}); err != nil {
		t.Fatalf("AppendPart of next segment failed: %s", err)
	}
	if p.tailSeqNo() != 5 {
		t.Errorf("next tail sequence: got %d, want 5", p.tailSeqNo())
	}
	if err = p.CommitSegment("79.m4s", 2.0, ""); err != nil {
		t.Fatalf("second CommitSegment failed: %s", err)
	}
	if p.Segments[p.last()].SeqId != 5 {
		t.Errorf("next committed segment SeqId: got %d, want 5", p.Segments[p.last()].SeqId)
	}
	if p.Encode().String() != p.Encode().String() {
		t.Errorf("two consecutive Encode calls differ")
	}

	// Clearing the hint is an explicit public operation too.
	if err = p.AppendPart(&Part{URI: "80.0.m4s", Duration: 0.5}); err != nil {
		t.Fatalf("AppendPart #3 failed: %s", err)
	}
	if err = p.SetPreloadHint(&PreloadHint{URI: "80.1.m4s"}); err != nil {
		t.Fatalf("SetPreloadHint #2 failed: %s", err)
	}
	p.ClearPreloadHint()
	if p.PreloadHint() != nil {
		t.Errorf("ClearPreloadHint did not remove the hint")
	}
	if strings.Contains(p.Encode().String(), "#EXT-X-PRELOAD-HINT") {
		t.Errorf("cleared hint is still encoded")
	}
}

// TestLLHLSPartRoundTrip decodes a hand written playlist with two
// complete segments, a tail and byte ranges and checks the structure
// and re-encoded tag attribution.
func TestLLHLSPartRoundTrip(t *testing.T) {
	src := strings.Join([]string{
		"#EXTM3U",
		"#EXT-X-VERSION:9",
		"#EXT-X-TARGETDURATION:2",
		"#EXT-X-MEDIA-SEQUENCE:10",
		"#EXT-X-PART-INF:PART-TARGET=0.5",
		"#EXT-X-KEY:METHOD=AES-128,URI=\"k1.bin\"",
		"#EXT-X-PART:DURATION=0.5,URI=\"f.m4s\",BYTERANGE=100@0,INDEPENDENT=YES",
		"#EXT-X-PART:DURATION=0.5,URI=\"f.m4s\",BYTERANGE=100",
		"#EXTINF:2,",
		"10.m4s",
		"#EXT-X-DISCONTINUITY",
		"#EXT-X-MAP:URI=\"init.mp4\"",
		"#EXT-X-PART:DURATION=0.5,URI=\"g.m4s\"",
		"#EXTINF:2,",
		"11.m4s",
		"#EXT-X-PART:DURATION=0.5,URI=\"h.m4s\",GAP=YES",
		"#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"h1.m4s\",BYTERANGE-START=200",
		"",
	}, "\n")

	p2, err := NewMediaPlaylist(3, 5)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err = p2.DecodeFrom(strings.NewReader(src), true); err != nil {
		t.Fatalf("DecodeFrom failed: %s", err)
	}

	if p2.Count() != 2 {
		t.Errorf("Count: got %d, want 2", p2.Count())
	}
	segs := p2.GetAllSegments()
	seg0, seg1 := segs[0], segs[1]
	if seg0.SeqId != 10 || seg1.SeqId != 11 {
		t.Errorf("segment SeqId: got %d/%d", seg0.SeqId, seg1.SeqId)
	}
	if len(seg0.Parts) != 2 {
		t.Fatalf("first segment parts: got %d, want 2", len(seg0.Parts))
	}
	part0, part1 := seg0.Parts[0], seg0.Parts[1]
	if part0.Limit != 100 || part0.Offset != 0 || !part0.HasRange || !part0.HasOffset || !part0.Independent {
		t.Errorf("first part range/flags mismatch: %+v", part0)
	}
	// The omitted offset continues the previous range of the same URI.
	if !part1.HasRange || !part1.HasOffset || part1.Offset != 100 || part1.Limit != 100 {
		t.Errorf("continued range mismatch: %+v", part1)
	}
	if seg0.Key == nil || seg0.Key.URI != "k1.bin" {
		t.Errorf("first segment key attribution: %+v", seg0.Key)
	}
	if !seg1.Discontinuity || seg1.Map == nil || seg1.Map.URI != "init.mp4" || len(seg1.Parts) != 1 || seg1.Parts[0].URI != "g.m4s" {
		t.Errorf("second segment declarations/parts mismatch: %+v", seg1)
	}

	tail := p2.TailParts()
	if len(tail) != 1 || tail[0].URI != "h.m4s" || !tail[0].Gap {
		t.Errorf("tail parts mismatch: %+v", tail)
	}
	hint := p2.PreloadHint()
	if hint == nil || hint.URI != "h1.m4s" || !hint.HasRange || hint.Offset != 200 || hint.HasLimit {
		t.Errorf("preload hint mismatch: %+v", hint)
	}
	if p2.PartTargetDuration != 0.5 {
		t.Errorf("PART-TARGET: got %v", p2.PartTargetDuration)
	}

	out := p2.Encode().String()
	// Continuation must be written with an explicit range: one round
	// trip cannot change the bytes a client downloads.
	if !strings.Contains(out, "#EXT-X-PART:DURATION=0.5,BYTERANGE=100@100,URI=\"f.m4s\"") {
		t.Errorf("continued byterange not explicit after round trip:\n%s", out)
	}
	for _, want := range []string{
		"#EXT-X-PART-INF:PART-TARGET=0.5",
		"#EXT-X-KEY:METHOD=AES-128",
		"#EXT-X-MAP:URI=\"init.mp4\"",
		"#EXT-X-DISCONTINUITY\n",
		"#EXT-X-PART:DURATION=0.5,URI=\"g.m4s\"",
		"#EXTINF:2.000,\n11.m4s",
		"#EXT-X-PART:DURATION=0.5,GAP=YES,URI=\"h.m4s\"",
		"#EXT-X-PRELOAD-HINT:TYPE=PART,BYTERANGE-START=200,URI=\"h1.m4s\"",
	} {
		if !strings.Contains(out, want) {
			t.Errorf("round trip output misses %q:\n%s", want, out)
		}
	}
	// The segment declarations stay ordered before their parts, and
	// the tail stays after the last complete segment.
	if !(indexOf(out, "init.mp4") < indexOf(out, "g.m4s") &&
		indexOf(out, "#EXT-X-DISCONTINUITY") < indexOf(out, "g.m4s") &&
		indexOf(out, "BYTERANGE=100@0") < indexOf(out, "BYTERANGE=100@100") &&
		indexOf(out, "BYTERANGE=100@100") < indexOf(out, "10.m4s") &&
		indexOf(out, "11.m4s") < indexOf(out, "h.m4s") &&
		indexOf(out, "h.m4s") < indexOf(out, "#EXT-X-PRELOAD-HINT")) {
		t.Errorf("round trip tag order wrong:\n%s", out)
	}
	if out != p2.Encode().String() {
		t.Errorf("two consecutive Encode calls differ")
	}

	// A second decode of the re-encoded playlist must be stable:
	// explicit ranges and tag attribution must not drift.
	p3, err := NewMediaPlaylist(3, 5)
	if err != nil {
		t.Fatalf("Create media playlist #3 failed: %s", err)
	}
	if err = p3.DecodeFrom(strings.NewReader(out), true); err != nil {
		t.Fatalf("second DecodeFrom failed: %s", err)
	}
	if again := p3.Encode().String(); again != out {
		t.Errorf("second round trip drifted:\nbefore:\n%s\nafter:\n%s", out, again)
	}
	if len(p3.TailParts()) != 1 || p3.PreloadHint() == nil || p3.Count() != 2 {
		t.Errorf("second round trip state mismatch: count=%d tail=%d hint=%v", p3.Count(), len(p3.TailParts()), p3.PreloadHint())
	}

	// A range without offset at the first part cannot be resolved.
	badStart := strings.Replace(src, "BYTERANGE=100@0", "BYTERANGE=100", 1)
	badPlaylist, _ := NewMediaPlaylist(3, 5)
	if err = badPlaylist.DecodeFrom(strings.NewReader(badStart), true); err != ErrPartRange {
		t.Errorf("first part range without offset: got %v, want ErrPartRange", err)
	}
	// Switching URI before a range without offset is an error too.
	badURI := strings.Replace(src, "#EXT-X-PART:DURATION=0.5,URI=\"f.m4s\",BYTERANGE=100\n",
		"#EXT-X-PART:DURATION=0.5,URI=\"other.m4s\",BYTERANGE=100\n", 1)
	badPlaylist2, _ := NewMediaPlaylist(3, 5)
	if err = badPlaylist2.DecodeFrom(strings.NewReader(badURI), true); err != ErrPartRange {
		t.Errorf("range continuation after URI change: got %v, want ErrPartRange", err)
	}
}

// TestLLHLSPartWindow drives a tiny window through several ring buffer
// wrap-arounds and checks parts leave with their segments and stale
// slots never come back.
func TestLLHLSPartWindow(t *testing.T) {
	const capacity = 2
	p, err := NewMediaPlaylist(capacity, capacity)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err = p.SetPartTargetDuration(0.5); err != nil {
		t.Fatalf("SetPartTargetDuration failed: %s", err)
	}

	const cycles = 6
	for cycle := 0; cycle < cycles; cycle++ {
		partURI := fmt.Sprintf("part%d-0.m4s", cycle)
		if err = p.AppendPart(&Part{URI: partURI, Duration: 0.5, Independent: true}); err != nil {
			t.Fatalf("cycle %d AppendPart failed: %s", cycle, err)
		}
		if cycle%2 == 1 {
			if err = p.AppendPart(&Part{URI: fmt.Sprintf("part%d-1.m4s", cycle), Duration: 0.5}); err != nil {
				t.Fatalf("cycle %d second AppendPart failed: %s", cycle, err)
			}
		}
		if p.Count() == capacity {
			if err = p.Remove(); err != nil {
				t.Fatalf("cycle %d Remove failed: %s", cycle, err)
			}
		}
		segURI := fmt.Sprintf("seg%d.m4s", cycle)
		if err = p.CommitSegment(segURI, 1.0, ""); err != nil {
			t.Fatalf("cycle %d CommitSegment failed: %s", cycle, err)
		}
		if p.Count() > capacity {
			t.Fatalf("cycle %d window grew to %d", cycle, p.Count())
		}
	}

	if p.Count() != capacity {
		t.Errorf("final Count: got %d, want %d", p.Count(), capacity)
	}
	// MEDIA-SEQUENCE advances per removed complete segment only.
	wantSeq := uint64(cycles - capacity)
	if p.SeqNo != wantSeq {
		t.Errorf("SeqNo: got %d, want %d", p.SeqNo, wantSeq)
	}

	segs := p.GetAllSegments()
	if len(segs) != capacity {
		t.Fatalf("window segments: got %d, want %d", len(segs), capacity)
	}
	for i, seg := range segs {
		cycle := cycles - capacity + i
		if seg.URI != fmt.Sprintf("seg%d.m4s", cycle) {
			t.Errorf("window slot %d URI: got %s, want seg%d.m4s", i, seg.URI, cycle)
		}
		if len(seg.Parts) == 0 || seg.Parts[0].URI != fmt.Sprintf("part%d-0.m4s", cycle) {
			t.Errorf("window slot %d lost its parts: %+v", i, seg.Parts)
		}
		if seg.SeqId != p.SeqNo+uint64(i) {
			t.Errorf("window slot %d SeqId: got %d, want %d", i, seg.SeqId, p.SeqNo+uint64(i))
		}
	}

	out := p.Encode().String()
	if strings.Contains(out, "part0-0.m4s") || strings.Contains(out, "seg0.m4s") ||
		strings.Contains(out, "part3-0.m4s") || strings.Contains(out, "seg3.m4s") {
		t.Errorf("slid-out parts/segments still encoded after wrap-around:\n%s", out)
	}
	if !strings.Contains(out, "part4-0.m4s") || !strings.Contains(out, "part5-0.m4s") {
		t.Errorf("current window parts missing after wrap-around:\n%s", out)
	}
	// Every encoded part still precedes its own EXTINF.
	lines := strings.Split(out, "\n")
	seenPart := false
	for _, line := range lines {
		switch {
		case strings.HasPrefix(line, "#EXT-X-PART:"):
			seenPart = true
		case strings.HasPrefix(line, "#EXTINF:"):
			if !seenPart {
				t.Errorf("EXTINF without preceding part:\n%s", out)
			}
			seenPart = false
		}
	}
	if !strings.Contains(out, fmt.Sprintf("#EXT-X-MEDIA-SEQUENCE:%d", wantSeq)) {
		t.Errorf("encoded MEDIA-SEQUENCE mismatch:\n%s", out)
	}
}

// TestLLHLSPartsControlPlainPlaylist keeps a playlist without parts on
// the legacy read/write path.
func TestLLHLSPartsControlPlainPlaylist(t *testing.T) {
	src := strings.Join([]string{
		"#EXTM3U",
		"#EXT-X-VERSION:3",
		"#EXT-X-TARGETDURATION:6",
		"#EXT-X-MEDIA-SEQUENCE:3",
		"#EXTINF:6.000,",
		"plain0.ts",
		"#EXTINF:6.000,",
		"plain1.ts",
		"",
	}, "\n")
	p, err := NewMediaPlaylist(2, 4)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err = p.DecodeFrom(strings.NewReader(src), true); err != nil {
		t.Fatalf("DecodeFrom failed: %s", err)
	}
	if p.Count() != 2 || p.TailParts() != nil || p.PreloadHint() != nil || p.PartTargetDuration != 0 {
		t.Errorf("plain playlist picked up LL-HLS state: %+v", p)
	}
	out := p.Encode().String()
	if strings.Contains(out, "#EXT-X-PART") || strings.Contains(out, "#EXT-X-PRELOAD-HINT") || strings.Contains(out, "#EXT-X-PART-INF") {
		t.Errorf("plain playlist gained LL-HLS tags:\n%s", out)
	}
	for _, seg := range p.GetAllSegments() {
		if len(seg.Parts) != 0 {
			t.Errorf("plain segment gained parts: %+v", seg)
		}
	}
	if !strings.Contains(out, "#EXTINF:6.000,\nplain0.ts") || !strings.Contains(out, "#EXTINF:6.000,\nplain1.ts") {
		t.Errorf("plain playlist output changed:\n%s", out)
	}
}
