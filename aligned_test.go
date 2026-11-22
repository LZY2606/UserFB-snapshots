package m3u8

/*
 Tests for wall-clock aligned assembly of media playlists.

 Copyright 2026 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this
 distribution and at https://github.com/grafov/m3u8/
*/

import (
	"math"
	"strings"
	"testing"
	"time"

	"github.com/grafov/m3u8/task_support/taskcheck"
)

var alignedBase = time.Date(2026, time.March, 1, 0, 0, 0, 0, time.UTC)

func wall(seconds int64) time.Time {
	return alignedBase.Add(time.Duration(seconds) * time.Second)
}

func wallNS(offset time.Duration) time.Time {
	return alignedBase.Add(offset)
}

type segSpec struct {
	uri    string
	title  string
	dur    float64
	pdt    *time.Time // nil means no PROGRAM-DATE-TIME anchor
	disc   bool
	m      *Map // nil means no EXT-X-MAP
	limit  int64
	offset int64
}

func buildPlaylist(t *testing.T, specs []segSpec) *MediaPlaylist {
	t.Helper()
	p, err := NewMediaPlaylist(0, uint(len(specs)))
	if err != nil {
		t.Fatalf("NewMediaPlaylist: %v", err)
	}
	for _, s := range specs {
		if err := p.Append(s.uri, s.dur, s.title); err != nil {
			t.Fatalf("Append %s: %v", s.uri, err)
		}
		if s.disc {
			if err := p.SetDiscontinuity(); err != nil {
				t.Fatalf("SetDiscontinuity: %v", err)
			}
		}
		if s.pdt != nil {
			if err := p.SetProgramDateTime(*s.pdt); err != nil {
				t.Fatalf("SetProgramDateTime: %v", err)
			}
		}
		if s.m != nil {
			if err := p.SetMap(s.m.URI, s.m.Limit, s.m.Offset); err != nil {
				t.Fatalf("SetMap: %v", err)
			}
		}
		if s.limit > 0 {
			if err := p.SetRange(s.limit, s.offset); err != nil {
				t.Fatalf("SetRange: %v", err)
			}
		}
	}
	p.Close()
	return p
}

func pdtAt(seconds int64) *time.Time {
	t := wall(seconds)
	return &t
}

func outputURIs(p *MediaPlaylist) []string {
	return uriList(p.GetAllSegments())
}

func uriList(segs []*MediaSegment) []string {
	out := make([]string, len(segs))
	for i, s := range segs {
		out[i] = s.URI
	}
	return out
}

// TestAlignedClocks concentrates on absolute-clock reconstruction:
// middle/last-segment anchors, propagation across discontinuity cycles,
// different segment densities and exact nanosecond arithmetic.
func TestAlignedClocks(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Video: two segments of 2s, anchor only on the second segment.
		video := buildPlaylist(t, []segSpec{
			{uri: "v0.ts", dur: 2},
			{uri: "v1.ts", dur: 2, pdt: pdtAt(2)},
		})
		// Audio: four segments of 1s, anchor on the first one.
		audio := buildPlaylist(t, []segSpec{
			{uri: "a0.ts", dur: 1, pdt: pdtAt(0)},
			{uri: "a1.ts", dur: 1},
			{uri: "a2.ts", dur: 1},
			{uri: "a3.ts", dur: 1},
		})
		origin := time.Date(2027, time.January, 2, 3, 4, 5, 0, time.UTC)

		out, err := AssembleAligned(
			map[string]*MediaPlaylist{"video": video, "audio": audio},
			[]WallRange{{Start: wall(2), End: wall(4)}},
			origin,
		)
		c.Check(err == nil, "cut [2s,4s) with middle anchor must succeed: %v", err)
		if err != nil {
			return
		}
		c.Check(out != nil, "success must return playlists")
		c.Check(len(out) == 2, "both tracks returned, got %d", len(out))
		c.Check(equalStrings(outputURIs(out["video"]), []string{"v1.ts"}), "video keeps one segment, got %v", outputURIs(out["video"]))
		c.Check(equalStrings(outputURIs(out["audio"]), []string{"a2.ts", "a3.ts"}), "audio keeps two segments, got %v", outputURIs(out["audio"]))

		vSegs := out["video"].GetAllSegments()
		c.Check(vSegs[0].ProgramDateTime.Equal(origin), "output PDT starts at origin, got %s", vSegs[0].ProgramDateTime)
		c.Check(!vSegs[0].Discontinuity, "first output segment carries no discontinuity")
		c.Check(out["video"].SeqNo == 0 && out["audio"].SeqNo == 0, "sequence numbers restart at 0")
		c.Check(out["video"].DiscontinuitySeq == 0 && out["audio"].DiscontinuitySeq == 0, "discontinuity sequence is 0")
		c.Check(out["video"].Closed && out["video"].MediaType == VOD, "output is closed VOD")

		// [1s,4s) cannot land on video's 2-second boundaries: the whole
		// assembly must fail without partial results.
		failed, failErr := AssembleAligned(
			map[string]*MediaPlaylist{"video": video, "audio": audio},
			[]WallRange{{Start: wall(1), End: wall(4)}},
			origin,
		)
		c.Check(failErr != nil, "cut [1s,4s) must fail for the 2s video track")
		c.Check(failed == nil, "failure must not return partial results, got %v", failed)
		c.Check(strings.Contains(failErr.Error(), "video"), "error names the failing track: %v", failErr)

		// Anchor in the middle of a cycle: times propagate both backward
		// and forward.
		mid := buildPlaylist(t, []segSpec{
			{uri: "m0.ts", dur: 2},
			{uri: "m1.ts", dur: 2, pdt: pdtAt(2)},
			{uri: "m2.ts", dur: 2},
		})
		midOut, err := AssembleAligned(
			map[string]*MediaPlaylist{"mid": mid},
			[]WallRange{{Start: wall(0), End: wall(6)}},
			origin,
		)
		c.Check(err == nil, "middle-anchor cycle reconstructs both ways: %v", err)
		if err != nil {
			return
		}
		midSegs := midOut["mid"].GetAllSegments()
		c.Check(midSegs[0].ProgramDateTime.Equal(origin), "first segment PDT derived backward")
		c.Check(midSegs[2].ProgramDateTime.Equal(origin.Add(4*time.Second)), "last segment PDT derived forward")

		// Two clock cycles split by a discontinuity. The second cycle only
		// carries an anchor on its last segment: times must be rebuilt
		// backward within that cycle, without borrowing the first anchor.
		twoCycle := buildPlaylist(t, []segSpec{
			{uri: "c0.ts", dur: 2, pdt: pdtAt(0)},
			{uri: "c1.ts", dur: 2},
			{uri: "c2.ts", dur: 2, disc: true},
			{uri: "c3.ts", dur: 2, pdt: pdtAt(6)},
		})
		cycOut, err := AssembleAligned(
			map[string]*MediaPlaylist{"cyc": twoCycle},
			[]WallRange{{Start: wall(2), End: wall(8)}},
			origin,
		)
		c.Check(err == nil, "cross-cycle cut with last-segment anchor works: %v", err)
		if err != nil {
			return
		}
		c.Check(equalStrings(outputURIs(cycOut["cyc"]), []string{"c1.ts", "c2.ts", "c3.ts"}), "cross-cycle chain selected")
		cycSegs := cycOut["cyc"].GetAllSegments()
		c.Check(!cycSegs[0].Discontinuity, "first output segment never carries a discontinuity")
		c.Check(cycSegs[1].Discontinuity, "source discontinuity on c2 is preserved")
		c.Check(!cycSegs[2].Discontinuity, "direct successor c3 needs no marker")
		c.Check(cycSegs[0].ProgramDateTime.Equal(origin) &&
			cycSegs[1].ProgramDateTime.Equal(origin.Add(2*time.Second)) &&
			cycSegs[2].ProgramDateTime.Equal(origin.Add(4*time.Second)),
			"cross-cycle PDT rewritten from origin")

		// A cycle without any anchor must fail even though the other cycle
		// has one.
		noAnchor := buildPlaylist(t, []segSpec{
			{uri: "n0.ts", dur: 2, pdt: pdtAt(0)},
			{uri: "n1.ts", dur: 2, disc: true},
			{uri: "n2.ts", dur: 2},
		})
		_, err = AssembleAligned(
			map[string]*MediaPlaylist{"noanchor": noAnchor},
			[]WallRange{{Start: wall(2), End: wall(4)}},
			origin,
		)
		c.Check(err != nil && strings.Contains(err.Error(), "noanchor"), "cycle without anchor rejected: %v", err)

		// Two anchors in one cycle must agree with the segment durations.
		conflict := buildPlaylist(t, []segSpec{
			{uri: "k0.ts", dur: 2, pdt: pdtAt(0)},
			{uri: "k1.ts", dur: 2, pdt: pdtAt(3)}, // should be t+2s
		})
		_, err = AssembleAligned(
			map[string]*MediaPlaylist{"conflict": conflict},
			[]WallRange{{Start: wall(0), End: wall(2)}},
			origin,
		)
		c.Check(err != nil && strings.Contains(err.Error(), "conflict"), "conflicting anchors rejected: %v", err)

		// Nanosecond-exact reconstruction using the prescribed
		// math.Round(seconds*1e9) rule.
		subSecond := time.Date(2026, time.May, 1, 0, 0, 0, 0, time.UTC)
		precise := buildPlaylist(t, []segSpec{
			{uri: "p0.ts", dur: 0.333333333, pdt: &subSecond},
			{uri: "p1.ts", dur: 0.001},
		})
		preciseOut, err := AssembleAligned(
			map[string]*MediaPlaylist{"precise": precise},
			[]WallRange{{Start: subSecond, End: subSecond.Add(334333333 * time.Nanosecond)}},
			origin,
		)
		c.Check(err == nil, "sub-second cut on exact nanosecond boundaries: %v", err)
		if err != nil {
			return
		}
		preciseSegs := preciseOut["precise"].GetAllSegments()
		c.Check(preciseSegs[1].ProgramDateTime.Equal(origin.Add(333333333*time.Nanosecond)),
			"0.333333333s rounds to 333333333ns, got %s", preciseSegs[1].ProgramDateTime.Sub(origin))
	})
}

// TestAlignedCuts covers cut ordering (repeated, reversed, non-adjacent),
// boundary/gap/overlap failures, input validation, unsupported features,
// deep-copy isolation and preservation of byte ranges and MAP switches.
func TestAlignedCuts(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		origin := time.Date(2027, time.June, 7, 8, 0, 0, 0, time.UTC)

		video := buildPlaylist(t, []segSpec{
			{uri: "v0.ts", dur: 2, pdt: pdtAt(0)},
			{uri: "v1.ts", dur: 2},
			{uri: "v2.ts", dur: 2},
			{uri: "v3.ts", dur: 2},
			{uri: "v4.ts", dur: 2},
		})
		audio := buildPlaylist(t, []segSpec{
			{uri: "a0.ts", dur: 1, pdt: pdtAt(0)},
			{uri: "a1.ts", dur: 1},
			{uri: "a2.ts", dur: 1},
			{uri: "a3.ts", dur: 1},
			{uri: "a4.ts", dur: 1},
			{uri: "a5.ts", dur: 1},
			{uri: "a6.ts", dur: 1},
			{uri: "a7.ts", dur: 1},
			{uri: "a8.ts", dur: 1},
			{uri: "a9.ts", dur: 1},
		})
		inputs := map[string]*MediaPlaylist{"video": video, "audio": audio}

		// Two non-adjacent cuts.
		out, err := AssembleAligned(inputs,
			[]WallRange{{Start: wall(0), End: wall(2)}, {Start: wall(6), End: wall(8)}},
			origin)
		c.Check(err == nil, "two non-adjacent cuts succeed: %v", err)
		if err != nil {
			return
		}
		c.Check(equalStrings(outputURIs(out["video"]), []string{"v0.ts", "v3.ts"}), "video chain order, got %v", outputURIs(out["video"]))
		c.Check(equalStrings(outputURIs(out["audio"]), []string{"a0.ts", "a1.ts", "a6.ts", "a7.ts"}), "audio chain order, got %v", outputURIs(out["audio"]))
		vSegs := out["video"].GetAllSegments()
		c.Check(!vSegs[0].Discontinuity && vSegs[1].Discontinuity, "jump between non-adjacent selections is marked")
		c.Check(vSegs[1].ProgramDateTime.Equal(origin.Add(2*time.Second)), "second selection PDT continues the new program clock")
		c.Check(out["video"].Count() == 2 && out["audio"].Count() == 4, "segment counts differ per density")

		// Equal total duration in nanoseconds even with different densities.
		var vTotal, aTotal int64
		for _, s := range out["video"].GetAllSegments() {
			vTotal += int64(math.Round(s.Duration * 1e9))
		}
		for _, s := range out["audio"].GetAllSegments() {
			aTotal += int64(math.Round(s.Duration * 1e9))
		}
		c.Check(vTotal == aTotal && vTotal == 4000000000, "both reels total 4s, got %d/%d", vTotal, aTotal)

		// Repeated selection: the same source chain is copied twice and the
		// copies must not share mutable state.
		repeat, err := AssembleAligned(inputs,
			[]WallRange{{Start: wall(2), End: wall(4)}, {Start: wall(2), End: wall(4)}},
			origin)
		c.Check(err == nil, "repeated cut succeeds: %v", err)
		if err != nil {
			return
		}
		c.Check(equalStrings(outputURIs(repeat["video"]), []string{"v1.ts", "v1.ts"}), "same segment selected twice")
		repSegs := repeat["video"].GetAllSegments()
		c.Check(repSegs[0] != repSegs[1], "repeated selections are distinct objects")
		c.Check(repSegs[1].Discontinuity, "second copy is not a direct successor")
		sourceBefore := video.GetAllSegments()[1].URI
		repSegs[0].URI = "mutated.ts"
		repeat["video"].ResetCache()
		c.Check(video.GetAllSegments()[1].URI == sourceBefore, "mutating an output copy leaves the source untouched")
		c.Check(repeat["video"].GetAllSegments()[1].URI == "v1.ts", "mutating one output copy leaves the other copy untouched")

		// Reversed selection order.
		reversed, err := AssembleAligned(inputs,
			[]WallRange{{Start: wall(4), End: wall(6)}, {Start: wall(0), End: wall(2)}},
			origin)
		c.Check(err == nil, "reversed cuts succeed: %v", err)
		if err != nil {
			return
		}
		c.Check(equalStrings(outputURIs(reversed["video"]), []string{"v2.ts", "v0.ts"}), "cuts are emitted in given order, got %v", outputURIs(reversed["video"]))
		revSegs := reversed["video"].GetAllSegments()
		c.Check(revSegs[0].ProgramDateTime.Equal(origin) && revSegs[1].ProgramDateTime.Equal(origin.Add(2*time.Second)),
			"reversed reel still runs forward from origin")

		// Gap in wall-clock time between adjacent segments of one track.
		gapAnchor := wall(5)
		gappy := buildPlaylist(t, []segSpec{
			{uri: "g0.ts", dur: 2, pdt: pdtAt(0)},
			{uri: "g1.ts", dur: 2, pdt: &gapAnchor}, // starts at t+5, leaving a 1s gap
		})
		_, err = AssembleAligned(map[string]*MediaPlaylist{"gap": gappy},
			[]WallRange{{Start: wall(0), End: wall(7)}}, origin)
		c.Check(err != nil && strings.Contains(err.Error(), "gap"), "wall-clock gap rejected: %v", err)

		// Overlap between adjacent segments.
		overlapAnchor := wall(1)
		overlap := buildPlaylist(t, []segSpec{
			{uri: "o0.ts", dur: 2, pdt: pdtAt(0)},
			{uri: "o1.ts", dur: 2, pdt: &overlapAnchor}, // starts at t+1 while previous ends t+2
		})
		_, err = AssembleAligned(map[string]*MediaPlaylist{"overlap": overlap},
			[]WallRange{{Start: wall(0), End: wall(3)}}, origin)
		c.Check(err != nil && strings.Contains(err.Error(), "overlap"), "wall-clock overlap rejected: %v", err)

		// Two clock cycles covering the same wall time: a cut boundary is
		// not unique.
		duplicated := buildPlaylist(t, []segSpec{
			{uri: "d0.ts", dur: 2, pdt: pdtAt(0)},
			{uri: "d1.ts", dur: 2, disc: true, pdt: pdtAt(0)},
			{uri: "d2.ts", dur: 2},
		})
		_, err = AssembleAligned(map[string]*MediaPlaylist{"dup": duplicated},
			[]WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(err != nil && strings.Contains(err.Error(), "dup"), "duplicate coverage across cycles rejected: %v", err)

		// Input validation.
		nilOut, err := AssembleAligned(nil, []WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(err != nil && nilOut == nil, "empty inputs rejected")
		nilOut, err = AssembleAligned(inputs, nil, origin)
		c.Check(err != nil && nilOut == nil, "empty cuts rejected")
		nilOut, err = AssembleAligned(map[string]*MediaPlaylist{"": video},
			[]WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(err != nil && nilOut == nil, "empty track name rejected")
		oldOrigin := time.Date(1969, time.December, 31, 0, 0, 0, 0, time.UTC)
		_, err = AssembleAligned(inputs, []WallRange{{Start: wall(0), End: wall(2)}}, oldOrigin)
		c.Check(err != nil, "origin before 1970 rejected: %v", err)
		future := time.Date(2100, time.January, 1, 0, 0, 0, 0, time.UTC)
		_, err = AssembleAligned(inputs, []WallRange{{Start: wall(0), End: wall(2)}}, future)
		c.Check(err != nil, "origin at/after 2100 rejected: %v", err)
		_, err = AssembleAligned(inputs, []WallRange{{Start: wall(2), End: wall(2)}}, origin)
		c.Check(err != nil, "empty cut interval rejected: %v", err)

		// Unsupported features are refused explicitly.
		encrypted, _ := NewMediaPlaylist(0, 1)
		_ = encrypted.Append("e0.ts", 2, "")
		_ = encrypted.SetDefaultKey("AES-128", "key.bin", "", "", "")
		encrypted.Close()
		_, err = AssembleAligned(map[string]*MediaPlaylist{"enc": encrypted},
			[]WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(err != nil && strings.Contains(err.Error(), "enc"), "EXT-X-KEY rejected: %v", err)

		withSCTE, _ := NewMediaPlaylist(0, 1)
		_ = withSCTE.Append("s0.ts", 2, "")
		_ = withSCTE.SetSCTE35(&SCTE{Cue: "cue"})
		withSCTE.Close()
		_, err = AssembleAligned(map[string]*MediaPlaylist{"scte": withSCTE},
			[]WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(err != nil, "SCTE-35 rejected: %v", err)

		iframe, _ := NewMediaPlaylist(0, 1)
		iframe.SetIframeOnly()
		_ = iframe.Append("i0.ts", 2, "")
		iframe.Close()
		_, err = AssembleAligned(map[string]*MediaPlaylist{"iframe": iframe},
			[]WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(err != nil, "I-frame-only rejected: %v", err)

		withCustom, _ := NewMediaPlaylist(0, 1)
		withCustom.SetCustomTag(&MockCustomTag{name: "#X-CUSTOM:", encodedString: "#X-CUSTOM:1"})
		_ = withCustom.Append("u0.ts", 2, "")
		withCustom.Close()
		_, err = AssembleAligned(map[string]*MediaPlaylist{"custom": withCustom},
			[]WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(err != nil, "custom tags rejected: %v", err)

		// MAP switch plus implicit byte ranges, parsed from a hand-written
		// playlist: ranges must be resolved with explicit offsets and both
		// MAP tags must survive encoding.
		raw := "#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n" +
			"#EXT-X-MAP:URI=\"init-a.mp4\",BYTERANGE=100@0\n" +
			"#EXT-X-PROGRAM-DATE-TIME:" + wall(0).Format(DATETIME) + "\n" +
			"#EXTINF:2,\n#EXT-X-BYTERANGE:10@0\nfile.mp4\n" +
			"#EXT-X-MAP:URI=\"init-b.mp4\"\n" +
			"#EXTINF:2,\n#EXT-X-BYTERANGE:12\nfile.mp4\n" +
			"#EXT-X-ENDLIST\n"
		parsed, perr := NewMediaPlaylist(0, 4)
		c.Check(perr == nil, "create ranged playlist: %v", perr)
		if perr != nil {
			return
		}
		perr = parsed.DecodeFrom(strings.NewReader(raw), true)
		c.Check(perr == nil, "parse implicit byterange and MAP switch: %v", perr)
		if perr != nil {
			return
		}
		rangedSegs := parsed.GetAllSegments()
		c.Check(rangedSegs[1].Offset == 10, "implicit offset resolved to 10, got %d", rangedSegs[1].Offset)
		c.Check(rangedSegs[1].Map.URI == "init-b.mp4", "segment MAP switch parsed")

		rangedOut, err := AssembleAligned(map[string]*MediaPlaylist{"ranged": parsed},
			[]WallRange{{Start: wall(0), End: wall(4)}}, origin)
		c.Check(err == nil, "cut across MAP switch succeeds: %v", err)
		if err != nil {
			return
		}
		encoded := rangedOut["ranged"].Encode().String()
		c.Check(strings.Contains(encoded, `#EXT-X-MAP:URI="init-a.mp4",BYTERANGE=100@0`), "first MAP retained explicitly")
		c.Check(strings.Contains(encoded, `#EXT-X-MAP:URI="init-b.mp4"`), "later MAP switch not swallowed")
		c.Check(strings.Contains(encoded, "#EXT-X-BYTERANGE:12@10"), "output writes explicit resolved offset, got:\n%s", encoded)
		c.Check(!strings.Contains(encoded, "#EXT-X-BYTERANGE:12\n"), "no implicit range syntax remains")

		// The source playlist and its cached encoding stay untouched.
		sourceEncoded := video.Encode().String()
		_, _ = AssembleAligned(inputs,
			[]WallRange{{Start: wall(0), End: wall(2)}}, origin)
		c.Check(video.Encode().String() == sourceEncoded, "source encoding cache unchanged")
		c.Check(video.Count() == 5, "source segment count unchanged, got %d", video.Count())

		// Strict media BYTERANGE rejection rules.
		mustReject := func(label, raw string) {
			bad, berr := NewMediaPlaylist(0, 4)
			c.Check(berr == nil, "%s: create playlist", label)
			berr = bad.DecodeFrom(strings.NewReader(raw), true)
			c.Check(berr != nil, "%s must be rejected", label)
		}
		baseHeader := "#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n"
		mustReject("implicit range with no predecessor",
			baseHeader+"#EXTINF:2,\n#EXT-X-BYTERANGE:10\nonly.ts\n#EXT-X-ENDLIST\n")
		mustReject("implicit range after a URI change",
			baseHeader+"#EXTINF:2,\n#EXT-X-BYTERANGE:10@0\na.ts\n"+
				"#EXTINF:2,\n#EXT-X-BYTERANGE:5\nb.ts\n#EXT-X-ENDLIST\n")
		mustReject("non-positive byterange length",
			baseHeader+"#EXTINF:2,\n#EXT-X-BYTERANGE:0@0\nx.ts\n#EXT-X-ENDLIST\n")
		mustReject("negative byterange offset",
			baseHeader+"#EXTINF:2,\n#EXT-X-BYTERANGE:10@-3\nx.ts\n#EXT-X-ENDLIST\n")
		mustReject("implicit MAP range with no prior MAP",
			"#EXTM3U\n#EXT-X-VERSION:6\n#EXT-X-TARGETDURATION:2\n"+
				"#EXT-X-MAP:URI=\"init.mp4\",BYTERANGE=12\n#EXTINF:2,\nx.ts\n#EXT-X-ENDLIST\n")

		// Explicit zero offset is legal and distinct from an omitted offset.
		explicitZero, ezErr := NewMediaPlaylist(0, 2)
		c.Check(ezErr == nil, "create explicit-zero playlist: %v", ezErr)
		ezErr = explicitZero.DecodeFrom(strings.NewReader(
			baseHeader+"#EXTINF:2,\n#EXT-X-BYTERANGE:10@0\nz.ts\n#EXT-X-ENDLIST\n"), true)
		c.Check(ezErr == nil && explicitZero.GetAllSegments()[0].Offset == 0,
			"explicit zero offset stays zero: %v", ezErr)

		// Once a MAP is in effect, a later segment without one is rejected by
		// the assembly even if constructed directly rather than parsed.
		mapDrop := buildPlaylist(t, []segSpec{
			{uri: "d0.ts", dur: 2, pdt: pdtAt(0), m: &Map{URI: "init.mp4"}},
			{uri: "d1.ts", dur: 2},
		})
		mapDrop.GetAllSegments()[1].Map = nil
		_, err = AssembleAligned(map[string]*MediaPlaylist{"mapdrop": mapDrop},
			[]WallRange{{Start: wall(0), End: wall(4)}}, origin)
		c.Check(err != nil && strings.Contains(err.Error(), "mapdrop"), "MAP dropped after being in effect is rejected: %v", err)
	})
}

// TestAlignedRoundTrip drives the full pipeline: wrapped ring windows,
// encode/decode round trips and a second-generation assembly, including a
// failed attempt followed by a successful one.
func TestAlignedRoundTrip(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		origin := time.Date(2028, time.August, 9, 10, 0, 0, 0, time.UTC)

		// Sliding window that has wrapped around its ring buffer: the
		// physical slots hold s3,s4,s2 but play order is s2,s3,s4.
		wrapped, err := NewMediaPlaylist(2, 3)
		c.Check(err == nil, "create wrapped playlist: %v", err)
		for _, u := range []string{"s0", "s1", "s2"} {
			c.Check(wrapped.Append(u, 1, "") == nil, "append %s", u)
		}
		c.Check(wrapped.Remove() == nil && wrapped.Remove() == nil, "slide window")
		c.Check(wrapped.Append("s3", 1, "") == nil && wrapped.Append("s4", 1, "") == nil, "wrap append")
		wrapped.Close()
		// The physical buffer has wrapped: slots hold s3,s4,s2 while ring
		// play order from the head is s2,s3,s4.
		c.Check(equalStrings(outputURIs(wrapped), []string{"s3", "s4", "s2"}),
			"GetAllSegments exposes the wrapped physical layout, got %v", outputURIs(wrapped))
		c.Check(equalStrings(uriList(playOrderSegments(wrapped)), []string{"s2", "s3", "s4"}),
			"head-based play order unwraps the ring, got %v", uriList(playOrderSegments(wrapped)))
		// Anchor the middle segment of the play-order window (s3) directly
		// through its ring slot, independent of the physical index.
		middleSlot := (wrapped.head + 1) % wrapped.capacity
		wrapped.Segments[middleSlot].ProgramDateTime = wall(1)

		other := buildPlaylist(t, []segSpec{
			{uri: "o0.ts", dur: 1, pdt: pdtAt(0)},
			{uri: "o1.ts", dur: 1},
			{uri: "o2.ts", dur: 1},
		})

		// A failed attempt first: t+0.5 is nobody's boundary.
		bad, badErr := AssembleAligned(
			map[string]*MediaPlaylist{"wrapped": wrapped, "other": other},
			[]WallRange{{Start: wallNS(500 * time.Millisecond), End: wall(2)}},
			origin)
		c.Check(badErr != nil && bad == nil, "failed attempt returns error and nil: %v", badErr)

		// Followed by a successful one on the same inputs.
		first, err := AssembleAligned(
			map[string]*MediaPlaylist{"wrapped": wrapped, "other": other},
			[]WallRange{{Start: wall(1), End: wall(3)}},
			origin)
		c.Check(err == nil, "success after failure on the same inputs: %v", err)
		if err != nil {
			return
		}
		c.Check(equalStrings(outputURIs(first["wrapped"]), []string{"s3", "s4"}),
			"wrapped window cut follows play order, got %v", outputURIs(first["wrapped"]))

		// Save (encode) and re-read (decode) every output, then check the
		// decoded playlist keeps the rebuilt clock.
		decoded := map[string]*MediaPlaylist{}
		for name, reel := range first {
			saved := reel.Encode().String()
			c.Check(strings.Contains(saved, "#EXT-X-ENDLIST"), "reel %s is closed VOD", name)
			c.Check(strings.Contains(saved, "#EXT-X-PLAYLIST-TYPE:VOD"), "reel %s marked VOD", name)
			c.Check(!strings.Contains(saved, "#EXT-X-DISCONTINUITY-SEQUENCE"), "discontinuity-sequence 0 is omitted")
			reread, derr := NewMediaPlaylist(0, reel.Count())
			c.Check(derr == nil, "allocate reread playlist: %v", derr)
			derr = reread.DecodeFrom(strings.NewReader(saved), true)
			c.Check(derr == nil, "strict decode of saved reel %s: %v", name, derr)
			c.Check(reread.Count() == reel.Count(), "reread segment count for %s", name)
			c.Check(reread.SeqNo == 0, "reread media sequence starts at 0")
			c.Check(reread.Closed && reread.MediaType == VOD, "reread reel stays closed VOD")
			origSegs := reel.GetAllSegments()
			rereadSegs := reread.GetAllSegments()
			for i, s := range rereadSegs {
				c.Check(s.URI == origSegs[i].URI && s.Duration == origSegs[i].Duration,
					"reel %s segment %d survives the round trip", name, i)
				c.Check(s.ProgramDateTime.Equal(origSegs[i].ProgramDateTime),
					"reel %s segment %d PDT survives: %s vs %s", name, i,
					s.ProgramDateTime, origSegs[i].ProgramDateTime)
				c.Check(s.Discontinuity == origSegs[i].Discontinuity,
					"reel %s segment %d discontinuity flag survives", name, i)
			}
			c.Check(!rereadSegs[0].Discontinuity, "reread first segment has no discontinuity")
			decoded[name] = reread
		}

		// Second-generation assembly on the saved-and-reread playlists,
		// using a repeated cut to exercise deep copies again.
		secondOrigin := origin.Add(24 * time.Hour)
		second, err := AssembleAligned(decoded,
			[]WallRange{
				{Start: origin, End: origin.Add(time.Second)},
				{Start: origin, End: origin.Add(time.Second)},
			},
			secondOrigin)
		c.Check(err == nil, "re-cut after save/re-read succeeds: %v", err)
		if err != nil {
			return
		}
		c.Check(second["wrapped"].Count() == 2 && second["other"].Count() == 2,
			"second generation selects one segment per copy, got %d/%d",
			second["wrapped"].Count(), second["other"].Count())
		secondSegs := second["wrapped"].GetAllSegments()
		c.Check(secondSegs[0].ProgramDateTime.Equal(secondOrigin), "new origin applied on re-cut")
		c.Check(secondSegs[1].ProgramDateTime.Equal(secondOrigin.Add(time.Second)),
			"second copy clock continues from new origin")
		c.Check(secondSegs[0] != secondSegs[1], "re-cut copies stay independent")

		// Final encode/decode of the second generation.
		for name, reel := range second {
			encoded := reel.Encode().String()
			final, ferr := NewMediaPlaylist(0, reel.Count())
			c.Check(ferr == nil, "alloc final reel: %v", ferr)
			c.Check(final.DecodeFrom(strings.NewReader(encoded), true) == nil,
				"second-generation reel %s decodes strictly", name)
			c.Check(final.Count() == reel.Count(), "final reel %s count", name)
		}
	})
}

func equalStrings(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
