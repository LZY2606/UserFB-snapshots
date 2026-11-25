/*
Package m3u8. Low-latency HLS partial segments (EXT-X-PART) tests.

See the AUTHORS and LICENSE files at the top-level directory of this distribution
and at https://github.com/grafov/m3u8/
*/
package m3u8

import (
	"fmt"
	"strings"
	"testing"
)

// findTagLines returns all lines of the encoded playlist that start with
// the given tag prefix, in output order.
func findTagLines(playlist, prefix string) []string {
	var lines []string
	for _, line := range strings.Split(playlist, "\n") {
		if strings.HasPrefix(line, prefix) {
			lines = append(lines, line)
		}
	}
	return lines
}

// TestLLHLSPartLifecycle exercises publishing parts before their
// complete segment exists and committing them into the playlist.
func TestLLHLSPartLifecycle(t *testing.T) {
	p, err := NewMediaPlaylist(6, 6)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := p.SetPartTarget(0.5); err != nil {
		t.Fatalf("Set part target failed: %s", err)
	}
	if err := p.Append("seg-0.m4s", 2.0, ""); err != nil {
		t.Fatalf("Append first segment failed: %s", err)
	}
	if err := p.Append("seg-1.m4s", 2.0, ""); err != nil {
		t.Fatalf("Append second segment failed: %s", err)
	}

	// Two complete segments, so the tail belongs to sequence number 2.
	if got := p.TailSequence(); got != 2 {
		t.Fatalf("Expected tail sequence 2, got %d", got)
	}
	firstPart := &Part{URI: "seg-2.part0.m4s", Duration: 0.5, Independent: true}
	if err := p.AppendPart(firstPart); err != nil {
		t.Fatalf("Append first part failed: %s", err)
	}
	// Mutating the caller's value must not change the published part.
	firstPart.URI = "mutated.m4s"
	firstPart.Duration = 99

	secondPart := &Part{URI: "seg-2.part1.m4s", Duration: 0.5}
	if err := p.AppendPart(secondPart); err != nil {
		t.Fatalf("Append second part failed: %s", err)
	}
	if p.Count() != 2 {
		t.Fatalf("Count must only include complete segments, got %d", p.Count())
	}
	if len(p.Parts) != 2 {
		t.Fatalf("Expected 2 tail parts, got %d", len(p.Parts))
	}
	if p.Parts[0].URI != "seg-2.part0.m4s" || p.Parts[1].URI != "seg-2.part1.m4s" {
		t.Fatalf("Tail parts were mutated through the caller's values: %+v", p.Parts)
	}
	for i, part := range p.Parts {
		if part.SeqId != 2 {
			t.Fatalf("Tail part %d expected SeqId 2, got %d", i, part.SeqId)
		}
	}

	hint := &PreloadHint{Type: "PART", URI: "seg-2.part2.m4s"}
	if err := p.SetPartPreloadHint(hint); err != nil {
		t.Fatalf("Set preload hint failed: %s", err)
	}
	hint.URI = "mutated-hint.m4s"
	if p.PreloadHint.URI != "seg-2.part2.m4s" {
		t.Fatalf("Preload hint was mutated through the caller's value: %+v", p.PreloadHint)
	}

	out := p.Encode().String()
	parts := findTagLines(out, "#EXT-X-PART:")
	if len(parts) != 2 {
		t.Fatalf("Expected 2 EXT-X-PART tags, got %d:\n%s", len(parts), out)
	}
	if !strings.Contains(parts[0], `URI="seg-2.part0.m4s"`) || !strings.Contains(parts[0], "INDEPENDENT=YES") {
		t.Fatalf("Unexpected first part tag: %s", parts[0])
	}
	if !strings.Contains(parts[1], `URI="seg-2.part1.m4s"`) {
		t.Fatalf("Unexpected second part tag: %s", parts[1])
	}
	if !strings.Contains(out, `#EXT-X-PRELOAD-HINT:TYPE=PART,URI="seg-2.part2.m4s"`) {
		t.Fatalf("Missing preload hint:\n%s", out)
	}

	// Commit the tail. The complete segment duration is caller controlled
	// and does not need to equal the sum of the part durations.
	if err := p.CommitParts("seg-2.m4s", 2.5, ""); err != nil {
		t.Fatalf("Commit parts failed: %s", err)
	}
	if p.Count() != 3 {
		t.Fatalf("Expected 3 complete segments after commit, got %d", p.Count())
	}
	if len(p.Parts) != 0 {
		t.Fatalf("Tail must be cleared after commit, got %d parts", len(p.Parts))
	}
	if p.PreloadHint != nil {
		t.Fatalf("Preload hint must be cleared after commit, got %+v", p.PreloadHint)
	}
	seg := p.Segments[2]
	if seg.URI != "seg-2.m4s" || seg.Duration != 2.5 {
		t.Fatalf("Unexpected committed segment: %+v", seg)
	}
	if len(seg.Parts) != 2 || seg.Parts[0].URI != "seg-2.part0.m4s" || seg.Parts[0].SeqId != 2 {
		t.Fatalf("Committed segment does not own the parts: %+v", seg.Parts)
	}

	// Output order: the committed segment's parts come before its EXTINF
	// and URI line, and after the segment declarations.
	out = p.Encode().String()
	part0At := strings.Index(out, `#EXT-X-PART:DURATION=0.5,URI="seg-2.part0.m4s"`)
	part1At := strings.Index(out, `#EXT-X-PART:DURATION=0.5,URI="seg-2.part1.m4s"`)
	extinfAt := strings.Index(out, "#EXTINF:2.500,")
	uriAt := strings.Index(out, "\nseg-2.m4s")
	if part0At < 0 || part1At < 0 || extinfAt < 0 || uriAt < 0 {
		t.Fatalf("Missing expected output lines after commit:\n%s", out)
	}
	if !(part0At < part1At && part1At < extinfAt && extinfAt < uriAt) {
		t.Fatalf("Parts are not ordered before their segment's EXTINF:\n%s", out)
	}

	// The next part belongs to the following media sequence number.
	if err := p.AppendPart(&Part{URI: "seg-3.part0.m4s", Duration: 0.5}); err != nil {
		t.Fatalf("Append part after commit failed: %s", err)
	}
	if got := p.TailSequence(); got != 3 {
		t.Fatalf("Expected tail sequence 3 after commit, got %d", got)
	}
	if p.Parts[0].SeqId != 3 {
		t.Fatalf("Expected next part SeqId 3, got %d", p.Parts[0].SeqId)
	}

	// Encode must be stable across repeated calls.
	first := p.Encode().String()
	second := p.Encode().String()
	if first != second {
		t.Fatalf("Two consecutive Encode calls differ:\n%s\n---\n%s", first, second)
	}

	// Tail declarations belong to the next segment. They appear after
	// the existing complete segments and before the tail parts, and the
	// commit moves them onto the new segment instead of leaving them on
	// a different one.
	newKey := &Key{Method: "AES-128", URI: "next-key.bin"}
	if err := p.SetTailKey(newKey.Method, newKey.URI, "", "", ""); err != nil {
		t.Fatalf("Set tail key failed: %s", err)
	}
	p.SetTailMap("next-init.mp4", 100, 0)
	p.SetTailDiscontinuity()
	tailOut := p.Encode().String()
	tailKeyAt := strings.Index(tailOut, `#EXT-X-KEY:METHOD=AES-128,URI="next-key.bin"`)
	tailMapAt := strings.Index(tailOut, `#EXT-X-MAP:URI="next-init.mp4"`)
	tailDiscAt := strings.Index(tailOut, "#EXT-X-DISCONTINUITY")
	tailPartAt := strings.Index(tailOut, `#EXT-X-PART:DURATION=0.5,URI="seg-3.part0.m4s"`)
	lastExtinfAt := strings.LastIndex(tailOut, "#EXTINF:")
	if tailKeyAt < 0 || tailMapAt < 0 || tailDiscAt < 0 || tailPartAt < 0 {
		t.Fatalf("Tail declarations missing:\n%s", tailOut)
	}
	// All tail declarations come after the last complete segment and
	// before the tail parts.
	if !(lastExtinfAt < tailKeyAt && lastExtinfAt < tailMapAt && lastExtinfAt < tailDiscAt &&
		tailKeyAt < tailPartAt && tailMapAt < tailPartAt && tailDiscAt < tailPartAt) {
		t.Fatalf("Tail declarations are not ordered before the tail parts:\n%s", tailOut)
	}
	if err := p.CommitParts("seg-3.m4s", 2.0, ""); err != nil {
		t.Fatalf("Commit tail declarations failed: %s", err)
	}
	committed := p.Segments[p.last()]
	if committed.Map == nil || committed.Map.URI != "next-init.mp4" {
		t.Fatalf("Tail map did not move onto the committed segment: %+v", committed.Map)
	}
	if !committed.Discontinuity {
		t.Fatalf("Tail discontinuity did not move onto the committed segment")
	}
	if p.TailMap != nil || p.TailDiscontinuity {
		t.Fatalf("Tail declarations survived the commit: %+v %v", p.TailMap, p.TailDiscontinuity)
	}
	afterCommit := p.Encode().String()
	// The committed segment's tag order: key, discontinuity, map, parts,
	// EXTINF. Decoding the same manifest must agree on the ownership.
	segOut := afterCommit[strings.Index(afterCommit, "seg-2.m4s\n")+len("seg-2.m4s\n"):]
	if !(strings.Index(segOut, "#EXT-X-KEY:") < strings.Index(segOut, "#EXT-X-DISCONTINUITY") &&
		strings.Index(segOut, "#EXT-X-DISCONTINUITY") < strings.Index(segOut, "#EXT-X-MAP:") &&
		strings.Index(segOut, "#EXT-X-MAP:") < strings.Index(segOut, "#EXT-X-PART:") &&
		strings.Index(segOut, "#EXT-X-PART:") < strings.LastIndex(segOut, "#EXTINF:")) {
		t.Fatalf("Committed declarations have the wrong order:\n%s", afterCommit)
	}
	decoded, err := NewMediaPlaylist(4, 8)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := decoded.DecodeFrom(strings.NewReader(afterCommit), true); err != nil {
		t.Fatalf("Decode after commit failed: %s", err)
	}
	if len(decoded.Parts) != 0 {
		t.Fatalf("Round trip invented tail parts: %+v", decoded.Parts)
	}
	if decoded.Segments[decoded.last()].Map == nil {
		t.Fatalf("Round trip lost the map on the committed segment")
	}

	// A failed commit must keep the previous tail and hint visible.
	if err := p.SetPartPreloadHint(&PreloadHint{Type: "PART", URI: "seg-3.part1.m4s"}); err != nil {
		t.Fatalf("Set preload hint failed: %s", err)
	}
	full, err := NewMediaPlaylist(1, 1)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := full.Append("only.m4s", 2.0, ""); err != nil {
		t.Fatalf("Append segment failed: %s", err)
	}
	if err := full.AppendPart(&Part{URI: "only.part0.m4s", Duration: 0.5}); err != nil {
		t.Fatalf("Append part to full playlist failed: %s", err)
	}
	if err := full.SetPartPreloadHint(&PreloadHint{Type: "PART", URI: "only.part1.m4s"}); err != nil {
		t.Fatalf("Set preload hint failed: %s", err)
	}
	if err := full.CommitParts("only2.m4s", 2.0, ""); err != ErrPlaylistFull {
		t.Fatalf("Expected ErrPlaylistFull from failed commit, got %v", err)
	}
	if len(full.Parts) != 1 || full.Parts[0].URI != "only.part0.m4s" {
		t.Fatalf("Failed commit lost the tail parts: %+v", full.Parts)
	}
	if full.PreloadHint == nil || full.PreloadHint.URI != "only.part1.m4s" {
		t.Fatalf("Failed commit lost the preload hint: %+v", full.PreloadHint)
	}
	fullOut := full.Encode().String()
	if !strings.Contains(fullOut, "#EXT-X-PART:") || !strings.Contains(fullOut, "#EXT-X-PRELOAD-HINT:") {
		t.Fatalf("Failed commit hid published content:\n%s", fullOut)
	}

	// A playlist without parts must encode exactly like before.
	plain, err := NewMediaPlaylist(3, 3)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := plain.Append("plain0.m4s", 2.0, ""); err != nil {
		t.Fatalf("Append segment failed: %s", err)
	}
	plainOut := plain.Encode().String()
	for _, forbidden := range []string{"#EXT-X-PART", "#EXT-X-PRELOAD-HINT", "#EXT-X-PART-INF"} {
		if strings.Contains(plainOut, forbidden) {
			t.Fatalf("Plain playlist unexpectedly contains %s:\n%s", forbidden, plainOut)
		}
	}
}

// TestLLHLSPartRoundTrip decodes a handwritten playlist with parts and
// checks byteranges and declaration ownership, then writes it back.
func TestLLHLSPartRoundTrip(t *testing.T) {
	// Same resource, different ranges. The second part omits its
	// offset and continues right after the first range.
	src := `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:2
#EXT-X-MEDIA-SEQUENCE:10
#EXT-X-PART-INF:PART-TARGET=0.5
#EXT-X-KEY:METHOD=AES-128,URI="key.bin"
#EXT-X-MAP:URI="init.mp4",BYTERANGE=720@0
#EXT-X-DISCONTINUITY
#EXT-X-PART:DURATION=0.5,URI="10.p0.m4s",BYTERANGE=1000@0,INDEPENDENT=YES
#EXT-X-PART:DURATION=0.5,URI="10.p0.m4s",BYTERANGE=2000
#EXTINF:2,
10.m4s
#EXT-X-PART:DURATION=0.5,URI="11.p0.m4s",GAP=YES
#EXT-X-PRELOAD-HINT:TYPE=PART,URI="11.p1.m4s"
`
	p, err := NewMediaPlaylist(4, 8)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := p.DecodeFrom(strings.NewReader(src), true); err != nil {
		t.Fatalf("Decode failed: %s", err)
	}
	if p.PartTarget != 0.5 {
		t.Fatalf("Expected PART-TARGET 0.5, got %v", p.PartTarget)
	}
	if p.Count() != 1 {
		t.Fatalf("Expected 1 complete segment, got %d", p.Count())
	}
	seg := p.Segments[0]
	if seg.SeqId != 10 || seg.URI != "10.m4s" {
		t.Fatalf("Unexpected complete segment: %+v", seg)
	}
	if len(seg.Parts) != 2 {
		t.Fatalf("Expected 2 parts on the complete segment, got %d", len(seg.Parts))
	}
	if seg.Parts[0].ByteRange == nil || seg.Parts[0].ByteRange.Offset != 0 || !seg.Parts[0].ByteRange.HasOffset {
		t.Fatalf("First part must keep its explicit zero offset: %+v", seg.Parts[0].ByteRange)
	}
	if seg.Parts[1].ByteRange == nil || seg.Parts[1].ByteRange.Offset != 1000 {
		t.Fatalf("Second part must continue the first part's range, got %+v", seg.Parts[1].ByteRange)
	}
	// The first segment inherits the declarations read before EXTINF.
	if seg.Key == nil || seg.Key.URI != "key.bin" {
		t.Fatalf("Key not attached to the complete segment: %+v", seg.Key)
	}
	if seg.Map == nil || seg.Map.URI != "init.mp4" {
		t.Fatalf("Map not attached to the complete segment: %+v", seg.Map)
	}
	if !seg.Discontinuity {
		t.Fatalf("Discontinuity not attached to the complete segment")
	}

	// Parts after the complete segment stay in the tail.
	if len(p.Parts) != 1 || p.Parts[0].SeqId != 11 {
		t.Fatalf("Unexpected tail: %+v", p.Parts)
	}
	if !p.Parts[0].Gap {
		t.Fatalf("GAP not decoded for the tail part: %+v", p.Parts[0])
	}
	if p.PreloadHint == nil || p.PreloadHint.URI != "11.p1.m4s" {
		t.Fatalf("Preload hint not decoded: %+v", p.PreloadHint)
	}
	// The preload hint is never treated as a finished part.
	for _, part := range p.Parts {
		if part.URI == "11.p1.m4s" {
			t.Fatalf("Preload hint leaked into the parts: %+v", p.Parts)
		}
	}

	out := p.Encode().String()
	// The key, map and discontinuity must precede the parts of their
	// segment, which in turn precede EXTINF.
	keyAt := strings.Index(out, "#EXT-X-KEY:")
	mapAt := strings.Index(out, "#EXT-X-MAP:")
	discAt := strings.Index(out, "#EXT-X-DISCONTINUITY")
	partAt := strings.Index(out, "#EXT-X-PART:")
	extinfAt := strings.Index(out, "#EXTINF:")
	if keyAt < 0 || mapAt < 0 || discAt < 0 || partAt < 0 || extinfAt < 0 {
		t.Fatalf("Missing declarations in output:\n%s", out)
	}
	if !(keyAt < mapAt && mapAt < discAt && discAt < partAt && partAt < extinfAt) {
		t.Fatalf("Declaration order changed:\n%s", out)
	}
	// Explicit ranges are written so a round trip cannot change the
	// downloaded bytes.
	if !strings.Contains(out, `URI="10.p0.m4s",BYTERANGE=1000@0`) {
		t.Fatalf("First part range not explicit:\n%s", out)
	}
	if !strings.Contains(out, `URI="10.p0.m4s",BYTERANGE=2000@1000`) {
		t.Fatalf("Second part range not resolved explicitly:\n%s", out)
	}
	if !strings.Contains(out, `#EXT-X-PART:DURATION=0.5,URI="11.p0.m4s",GAP=YES`) {
		t.Fatalf("GAP part not encoded:\n%s", out)
	}

	// Reading the re-encoded playlist must produce the same ranges.
	p2, err := NewMediaPlaylist(4, 8)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := p2.DecodeFrom(strings.NewReader(out), true); err != nil {
		t.Fatalf("Decode of re-encoded playlist failed: %s", err)
	}
	if p2.Segments[0].Parts[1].ByteRange.Offset != 1000 || p2.Segments[0].Parts[1].ByteRange.Limit != 2000 {
		t.Fatalf("Round trip changed the second part range: %+v", p2.Segments[0].Parts[1].ByteRange)
	}
	if !strings.Contains(p2.Encode().String(), `BYTERANGE=2000@1000`) {
		t.Fatalf("Round trip changed the encoded range:\n%s", p2.Encode().String())
	}

	// Omitted offsets are only valid for the same URI within the media
	// sequence and with a determinable predecessor.
	bad := `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:2
#EXT-X-MEDIA-SEQUENCE:10
#EXT-X-PART:DURATION=0.5,URI="a.m4s",BYTERANGE=1000
`
	badP, err := NewMediaPlaylist(4, 8)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := badP.DecodeFrom(strings.NewReader(bad), true); err == nil {
		t.Fatalf("Expected error for an offset-less first part")
	}
	changed := `#EXTM3U
#EXT-X-VERSION:9
#EXT-X-TARGETDURATION:2
#EXT-X-MEDIA-SEQUENCE:10
#EXT-X-PART:DURATION=0.5,URI="a.m4s",BYTERANGE=1000@0
#EXT-X-PART:DURATION=0.5,URI="b.m4s",BYTERANGE=2000
`
	changedP, err := NewMediaPlaylist(4, 8)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := changedP.DecodeFrom(strings.NewReader(changed), true); err == nil {
		t.Fatalf("Expected error when the URI changes without an offset")
	}
}

// TestLLHLSPartWindow slides a small window repeatedly around the ring
// buffer and checks that parts are not resurrected or miscounted.
func TestLLHLSPartWindow(t *testing.T) {
	// Three complete segments fit the window; the tail never counts as
	// one of them even though the ring buffer has no spare slot.
	p, err := NewMediaPlaylist(3, 3)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	if err := p.SetPartTarget(0.5); err != nil {
		t.Fatalf("Set part target failed: %s", err)
	}
	if err := p.Append("seg-0.m4s", 2.0, ""); err != nil {
		t.Fatalf("Append segment failed: %s", err)
	}

	// Publish a part, then commit its complete segment while sliding the
	// window. Repeated enough times to wrap the ring buffer slots many
	// times over.
	for i := 1; i <= 8; i++ {
		before := p.Count()
		if err := p.AppendPart(&Part{URI: fmt.Sprintf("seg-%d.p0.m4s", i), Duration: 0.5}); err != nil {
			t.Fatalf("Round %d: append part failed: %s", i, err)
		}
		// Count only counts complete segments; the tail is not slidable
		// and MEDIA-SEQUENCE does not move because of it.
		if p.Count() != before {
			t.Fatalf("Round %d: Count changed when appending a part: %d -> %d", i, before, p.Count())
		}
		if got := p.TailSequence(); got != uint64(i) {
			t.Fatalf("Round %d: expected tail sequence %d, got %d", i, i, got)
		}
		if p.Parts[len(p.Parts)-1].SeqId != uint64(i) {
			t.Fatalf("Round %d: part belongs to sequence %d, got %d", i, i, p.Parts[len(p.Parts)-1].SeqId)
		}
		// Free a slot exactly like a caller sliding complete segments.
		if p.Count() >= p.WinSize() {
			if err := p.Remove(); err != nil {
				t.Fatalf("Round %d: remove failed: %s", i, err)
			}
		}
		if err := p.CommitParts(fmt.Sprintf("seg-%d.m4s", i), 2.0, ""); err != nil {
			t.Fatalf("Round %d: commit failed: %s", i, err)
		}
		if len(p.Parts) != 0 {
			t.Fatalf("Round %d: tail not cleared: %+v", i, p.Parts)
		}

		segments := ringURIs(p)
		wantVisible := i + 1
		if wantVisible > 3 {
			wantVisible = 3
		}
		if len(segments) != wantVisible || len(segments) != int(p.Count()) {
			t.Fatalf("Round %d: expected %d visible complete segments, got %d/%d", i, wantVisible, len(segments), p.Count())
		}
		firstVisible := i - 2
		if firstVisible < 0 {
			firstVisible = 0
		}
		for j := 0; j < len(segments); j++ {
			wantURI := fmt.Sprintf("seg-%d.m4s", firstVisible+j)
			if segments[j] != wantURI {
				t.Fatalf("Round %d: segment %d expected %s, got %s", i, j, wantURI, segments[j])
			}
		}
		// Every visible segment committed from parts owns exactly its own
		// part; parts of slid-out segments are gone with them.
		for j := 0; j < len(segments); j++ {
			seg := p.Segments[(p.head+uint(j))%p.capacity]
			wantParts := 0
			if firstVisible+j >= 1 {
				wantParts = 1
			}
			if len(seg.Parts) != wantParts {
				t.Fatalf("Round %d: segment %s expected %d parts, got %d", i, fmt.Sprintf("seg-%d.m4s", firstVisible+j), wantParts, len(seg.Parts))
			}
			for _, part := range seg.Parts {
				if part.URI != fmt.Sprintf("seg-%d.p0.m4s", seg.SeqId) || part.SeqId != seg.SeqId {
					t.Fatalf("Round %d: segment %s carries a foreign part %+v", i, seg.URI, part)
				}
			}
		}

		out := p.Encode().String()
		visibleSeqs := make(map[uint64]bool)
		for j := 0; j < int(p.Count()); j++ {
			visibleSeqs[p.Segments[(p.head+uint(j))%p.capacity].SeqId] = true
		}
		for _, line := range findTagLines(out, "#EXT-X-PART:") {
			uri := partURIAttribute(line)
			seq, ok := partSeqFromURI(uri)
			// A staged tail part belongs to the next sequence; complete
			// segment parts must belong to a visible complete segment.
			isTailPart := uri == fmt.Sprintf("seg-%d.p0.m4s", i+1)
			if !ok || (!visibleSeqs[seq] && !isTailPart) {
				t.Fatalf("Round %d: stale part tag reappeared: %s\n%s", i, line, out)
			}
		}
	}

	// Stage one last tail part, then try to commit while the ring is
	// full. The commit fails and everything already published stays
	// visible: tail parts, preload hint and complete segments.
	last := 9
	if err := p.AppendPart(&Part{URI: fmt.Sprintf("seg-%d.p0.m4s", last), Duration: 0.5}); err != nil {
		t.Fatalf("Append final part failed: %s", err)
	}
	if err := p.SetPartPreloadHint(&PreloadHint{Type: "PART", URI: fmt.Sprintf("seg-%d.p1.m4s", last)}); err != nil {
		t.Fatalf("Set preload hint failed: %s", err)
	}
	countBefore := p.Count()
	seqBefore := p.SeqNo
	if err := p.CommitParts(fmt.Sprintf("seg-%d.m4s", last), 2.0, ""); err != ErrPlaylistFull {
		t.Fatalf("Expected ErrPlaylistFull for a full buffer, got %v", err)
	}
	if p.Count() != countBefore || p.SeqNo != seqBefore {
		t.Fatalf("Failed commit changed complete segments: count %d->%d seq %d->%d", countBefore, p.Count(), seqBefore, p.SeqNo)
	}
	if len(p.Parts) != 1 || p.Parts[0].URI != fmt.Sprintf("seg-%d.p0.m4s", last) {
		t.Fatalf("Failed commit lost the tail parts: %+v", p.Parts)
	}
	if p.PreloadHint == nil || p.PreloadHint.URI != fmt.Sprintf("seg-%d.p1.m4s", last) {
		t.Fatalf("Failed commit lost the preload hint: %+v", p.PreloadHint)
	}
	failedOut := p.Encode().String()
	// Every visible complete segment keeps its part, plus the tail part
	// staged for the failed commit.
	if got := strings.Count(failedOut, "#EXT-X-PART:"); got != int(p.Count())+1 {
		t.Fatalf("Expected %d EXT-X-PART tags after the failed commit, got %d:\n%s", p.Count()+1, got, failedOut)
	}
	if !strings.Contains(failedOut, fmt.Sprintf("#EXT-X-PART:DURATION=0.5,URI=\"seg-%d.p0.m4s\"", last)) ||
		!strings.Contains(failedOut, fmt.Sprintf("#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"seg-%d.p1.m4s\"", last)) {
		t.Fatalf("Failed commit hid already published content:\n%s", failedOut)
	}

	// The caller can still make progress by sliding one complete
	// segment, keeping the tail and hint, and then committing.
	if err := p.Remove(); err != nil {
		t.Fatalf("Remove failed: %s", err)
	}
	if p.Parts[0].SeqId != uint64(last) {
		t.Fatalf("Tail part expected SeqId %d after slide, got %d", last, p.Parts[0].SeqId)
	}
	if err := p.CommitParts(fmt.Sprintf("seg-%d.m4s", last), 2.0, ""); err != nil {
		t.Fatalf("Commit after slide failed: %s", err)
	}
	if len(p.Parts) != 0 || p.PreloadHint != nil {
		t.Fatalf("Commit after slide did not clear the tail: %+v %+v", p.Parts, p.PreloadHint)
	}
	if p.Count() != 3 {
		t.Fatalf("Expected 3 complete segments after final commit, got %d", p.Count())
	}
	if p.SeqNo != uint64(7) {
		t.Fatalf("Expected MEDIA-SEQUENCE 7, got %d", p.SeqNo)
	}
	segments := ringURIs(p)
	if len(segments) != 3 || segments[0] != "seg-7.m4s" || segments[2] != fmt.Sprintf("seg-%d.m4s", last) {
		t.Fatalf("Unexpected final window: %v", segments)
	}

	// A closed playlist never slides, so a commit into a full buffer
	// must fail without touching the published tail.
	closed, err := NewMediaPlaylist(2, 2)
	if err != nil {
		t.Fatalf("Create media playlist failed: %s", err)
	}
	closed.Close()
	if err := closed.Append("a.m4s", 2.0, ""); err != nil {
		t.Fatalf("Append failed: %s", err)
	}
	if err := closed.Append("b.m4s", 2.0, ""); err != nil {
		t.Fatalf("Append failed: %s", err)
	}
	if err := closed.AppendPart(&Part{URI: "tail.m4s", Duration: 0.5}); err != nil {
		t.Fatalf("Append part failed: %s", err)
	}
	if err := closed.CommitParts("c.m4s", 2.0, ""); err != ErrPlaylistFull {
		t.Fatalf("Expected ErrPlaylistFull, got %v", err)
	}
	if len(closed.Parts) != 1 || closed.Parts[0].URI != "tail.m4s" {
		t.Fatalf("Tail lost on failed commit: %+v", closed.Parts)
	}
}

// ringURIs returns the URIs of the segments visible in the sliding
// window, independent of the ring buffer slot positions.
func ringURIs(p *MediaPlaylist) []string {
	uris := make([]string, 0, p.count)
	idx := p.head
	for i := uint(0); i < p.count; i++ {
		seg := p.Segments[idx]
		idx = (idx + 1) % p.capacity
		if seg == nil {
			continue
		}
		uris = append(uris, seg.URI)
	}
	return uris
}

// partURIAttribute extracts the URI attribute of an EXT-X-PART tag.
func partURIAttribute(line string) string {
	const marker = `URI="`
	i := strings.Index(line, marker)
	if i < 0 {
		return ""
	}
	rest := line[i+len(marker):]
	if j := strings.Index(rest, `"`); j >= 0 {
		return rest[:j]
	}
	return ""
}

// partSeqFromURI parses the segment number from "seg-N.p0.m4s" URIs.
func partSeqFromURI(uri string) (uint64, bool) {
	var seq uint64
	if _, err := fmt.Sscanf(uri, "seg-%d.p0.m4s", &seq); err != nil {
		return 0, false
	}
	return seq, true
}
