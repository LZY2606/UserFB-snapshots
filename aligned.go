package m3u8

/*
 Part of M3U8 parser & generator library.
 This file implements wall-clock aligned assembly of several media
 playlists into one highlight reel without downloading or re-encoding any
 media segment.

 Copyright 2026 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this
 distribution and at https://github.com/grafov/m3u8/
*/

import (
	"fmt"
	"math"
	"sort"
	"time"
)

// WallRange is a half-open interval [Start, End) expressed in absolute
// program time (EXT-X-PROGRAM-DATE-TIME clock).
type WallRange struct {
	Start time.Time
	End   time.Time
}

var (
	wallClockMin = time.Date(1970, time.January, 1, 0, 0, 0, 0, time.UTC)
	wallClockMax = time.Date(2100, time.January, 1, 0, 0, 0, 0, time.UTC)
)

func withinWallClock(t time.Time) bool {
	return !t.Before(wallClockMin) && t.Before(wallClockMax)
}

// playOrderSegments returns the complete segments of a playlist in current
// ring-window play order, starting at the ring head and wrapping around.
// A sliding playlist may have wrapped its physical buffer, so physical
// slice indices and GetAllSegments' concatenation order are not the play
// order.
func playOrderSegments(p *MediaPlaylist) []*MediaSegment {
	if p.count == 0 {
		return nil
	}
	out := make([]*MediaSegment, 0, p.count)
	idx := p.head
	for i := uint(0); i < p.count; i++ {
		out = append(out, p.Segments[idx])
		idx = (idx + 1) % p.capacity
	}
	return out
}

// timedSegment is one media segment with its reconstructed absolute
// [start, end) interval.
type timedSegment struct {
	segment    *MediaSegment
	playIndex  int
	start      time.Time
	end        time.Time
	durationNS int64
}

// clockCycle groups the segments between two EXT-X-DISCONTINUITY markers.
// Every cycle carries its own program-date-time clock and must contain at
// least one PROGRAM-DATE-TIME anchor.
type clockCycle struct {
	segments []*timedSegment
}

// trackClock is the reconstructed wall-clock timeline of one playlist.
type trackClock struct {
	track  string
	play   []*MediaSegment // segments in current ring-window play order
	cycles []*clockCycle
	timed  []*timedSegment // all segments, play order, with absolute times
}

// segmentDurationNS converts an EXTINF duration (seconds) to nanoseconds
// using the required rounding rule.
func segmentDurationNS(seconds float64) (int64, error) {
	if math.IsNaN(seconds) || math.IsInf(seconds, 0) || seconds <= 0 {
		return 0, fmt.Errorf("segment duration must be a positive finite number, got %v", seconds)
	}
	ns := int64(math.Round(seconds * 1e9))
	if ns <= 0 {
		return 0, fmt.Errorf("segment duration rounded to a non-positive nanosecond span: %v", seconds)
	}
	return ns, nil
}

// rejectUnsupported makes sure the playlist only uses features the
// assembly pipeline knows how to rewrite.
func rejectUnsupported(track string, p *MediaPlaylist) error {
	if p.Iframe {
		return fmt.Errorf("track %q: I-frame-only playlists are not supported", track)
	}
	if p.Key != nil {
		return fmt.Errorf("track %q: encrypted media (EXT-X-KEY) is not supported", track)
	}
	if p.WV != nil {
		return fmt.Errorf("track %q: Widevine tags are not supported", track)
	}
	if len(p.Custom) > 0 {
		return fmt.Errorf("track %q: custom playlist tags are not supported", track)
	}
	for i, seg := range playOrderSegments(p) {
		if seg == nil {
			return fmt.Errorf("track %q: segment #%d is empty", track, i)
		}
		if seg.Key != nil {
			return fmt.Errorf("track %q: encrypted media (EXT-X-KEY) is not supported", track)
		}
		if seg.SCTE != nil {
			return fmt.Errorf("track %q: SCTE-35 tags are not supported", track)
		}
		if len(seg.Custom) > 0 {
			return fmt.Errorf("track %q: custom segment tags are not supported", track)
		}
	}
	return nil
}

// buildTrackClock reads the complete segments of one playlist in ring
// window play order, splits them at discontinuities and reconstructs every
// segment's absolute start/end time from the PROGRAM-DATE-TIME anchors of
// its own clock cycle.
func buildTrackClock(track string, p *MediaPlaylist) (*trackClock, error) {
	play := playOrderSegments(p)
	if len(play) == 0 {
		return nil, fmt.Errorf("track %q: playlist has no segments", track)
	}

	tc := &trackClock{track: track, play: play}

	// Split play order into cycles on discontinuity markers.
	var cycle *clockCycle
	mapStarted := false
	for i, seg := range play {
		if seg.Discontinuity && i > 0 {
			cycle = nil
		}
		if cycle == nil {
			cycle = &clockCycle{}
			tc.cycles = append(tc.cycles, cycle)
		}
		dur, err := segmentDurationNS(seg.Duration)
		if err != nil {
			return nil, fmt.Errorf("track %q segment #%d (%q): %v", track, i, seg.URI, err)
		}
		ts := &timedSegment{segment: seg, playIndex: i, durationNS: dur}
		cycle.segments = append(cycle.segments, ts)
		tc.timed = append(tc.timed, ts)
		// Once an initialization section is in effect it may not disappear.
		if seg.Map != nil {
			mapStarted = true
		} else if mapStarted {
			return nil, fmt.Errorf("track %q segment #%d (%q): segment without EXT-X-MAP after an initialization section was already in effect", track, i, seg.URI)
		}
	}

	// Reconstruct each cycle from its own anchor(s).
	for ci, cyc := range tc.cycles {
		anchorCount := 0
		for _, ts := range cyc.segments {
			if !ts.segment.ProgramDateTime.IsZero() {
				anchorCount++
			}
		}
		if anchorCount == 0 {
			return nil, fmt.Errorf("track %q clock cycle #%d: cycle has no EXT-X-PROGRAM-DATE-TIME anchor", track, ci)
		}

		// First anchor seeds the clock; durations propagate backward to
		// every earlier start, then forward again to fill every end time.
		firstAnchor := -1
		for i, ts := range cyc.segments {
			if !ts.segment.ProgramDateTime.IsZero() {
				firstAnchor = i
				ts.start = ts.segment.ProgramDateTime
				break
			}
		}
		for i := firstAnchor - 1; i >= 0; i-- {
			cur := cyc.segments[i]
			next := cyc.segments[i+1]
			cur.start = addNS(next.start, -cur.durationNS)
		}
		for i := 0; i < len(cyc.segments); i++ {
			cur := cyc.segments[i]
			cur.end = addNS(cur.start, cur.durationNS)
			if i+1 < len(cyc.segments) {
				cyc.segments[i+1].start = cur.end
			}
		}

		// All anchors of the cycle must agree with the accumulated
		// durations, and neighboring segments must be contiguous in wall
		// clock time.
		for i, ts := range cyc.segments {
			if !ts.segment.ProgramDateTime.IsZero() && !ts.segment.ProgramDateTime.Equal(ts.start) {
				return nil, fmt.Errorf("track %q clock cycle #%d segment #%d (%q): conflicting PROGRAM-DATE-TIME anchor %s, expected %s",
					track, ci, ts.playIndex, ts.segment.URI,
					ts.segment.ProgramDateTime.UTC().Format(time.RFC3339Nano),
					ts.start.UTC().Format(time.RFC3339Nano))
			}
			if !withinWallClock(ts.start) || !withinWallClock(ts.end) {
				return nil, fmt.Errorf("track %q segment #%d (%q): absolute time outside the 1970..2100 range", track, ts.playIndex, ts.segment.URI)
			}
			if i > 0 {
				prev := cyc.segments[i-1]
				if !prev.end.Equal(ts.start) {
					if ts.start.Before(prev.end) {
						return nil, fmt.Errorf("track %q clock cycle #%d: segments %q and %q overlap in program time", track, ci, prev.segment.URI, ts.segment.URI)
					}
					return nil, fmt.Errorf("track %q clock cycle #%d: gap in program time between %q and %q", track, ci, prev.segment.URI, ts.segment.URI)
				}
			}
		}
	}
	return tc, nil
}

func addNS(t time.Time, ns int64) time.Time {
	return t.Add(time.Duration(ns))
}

// addChecked accumates a positive nanosecond span and fails if the running
// total would overflow int64.
func addChecked(total, ns int64) (int64, error) {
	if ns > math.MaxInt64-total {
		return 0, fmt.Errorf("accumulated duration overflows int64 nanoseconds (total %d + %d)", total, ns)
	}
	return total + ns, nil
}

// matchCut resolves one wall-clock cut to a single contiguous chain of
// complete segments in play order.
func (tc *trackClock) matchCut(cut WallRange, cutIndex int) ([]*timedSegment, error) {
	var startSeg, endSeg *timedSegment
	startHits, endHits := 0, 0
	for _, ts := range tc.timed {
		if ts.start.Equal(cut.Start) {
			startSeg = ts
			startHits++
		}
		if ts.end.Equal(cut.End) {
			endSeg = ts
			endHits++
		}
	}
	if startHits == 0 || startHits > 1 {
		return nil, fmt.Errorf("track %q cut #%d [%s,%s): cut start does not land on a unique segment boundary",
			tc.track, cutIndex, cut.Start.UTC().Format(time.RFC3339Nano), cut.End.UTC().Format(time.RFC3339Nano))
	}
	if endHits == 0 || endHits > 1 {
		return nil, fmt.Errorf("track %q cut #%d [%s,%s): cut end does not land on a unique segment boundary",
			tc.track, cutIndex, cut.Start.UTC().Format(time.RFC3339Nano), cut.End.UTC().Format(time.RFC3339Nano))
	}
	if endSeg.playIndex < startSeg.playIndex {
		return nil, fmt.Errorf("track %q cut #%d: cut does not follow a forward play-order segment chain",
			tc.track, cutIndex)
	}

	chain := make([]*timedSegment, 0, endSeg.playIndex-startSeg.playIndex+1)
	cursor := startSeg
	for {
		chain = append(chain, cursor)
		if cursor.playIndex == endSeg.playIndex {
			break
		}
		if cursor.playIndex+1 >= len(tc.timed) {
			return nil, fmt.Errorf("track %q cut #%d: cut end is not reachable along the play-order segment chain", tc.track, cutIndex)
		}
		next := tc.timed[cursor.playIndex+1]
		if !next.start.Equal(cursor.end) {
			if next.start.Before(cursor.end) {
				return nil, fmt.Errorf("track %q cut #%d: overlapping segments inside the cut around %q", tc.track, cutIndex, cursor.segment.URI)
			}
			return nil, fmt.Errorf("track %q cut #%d: gap in program time inside the cut around %q", tc.track, cutIndex, cursor.segment.URI)
		}
		cursor = next
	}
	if !cursor.end.Equal(cut.End) {
		return nil, fmt.Errorf("track %q cut #%d: cut end does not match the end of a complete segment chain", tc.track, cutIndex)
	}

	// Any other segment covering part of the cut means a different cycle
	// duplicates program time inside the selection.
	for _, ts := range tc.timed {
		if ts.start.Before(cut.End) && cut.Start.Before(ts.end) {
			if ts.playIndex < startSeg.playIndex || ts.playIndex > endSeg.playIndex {
				return nil, fmt.Errorf("track %q cut #%d: segment %q from another clock cycle duplicates time inside the cut",
					tc.track, cutIndex, ts.segment.URI)
			}
		}
	}
	return chain, nil
}

// deepCopySegment duplicates a selected segment together with its
// initialization section, so repeated selections never share mutable
// state.
func deepCopySegment(src *MediaSegment) *MediaSegment {
	cp := *src
	if src.Map != nil {
		m := *src.Map
		cp.Map = &m
	}
	cp.Key = nil
	cp.SCTE = nil
	cp.Custom = nil
	return &cp
}

// AssembleAligned cuts every input media playlist at the same absolute
// program-time boundaries and rebuilds them as closed VOD playlists whose
// program clocks run continuously from origin. No media is downloaded or
// re-encoded: every selection is a chain of complete source segments.
//
// The returned map mirrors the input track names. On any failure it
// returns nil and an error naming the offending track and cut.
func AssembleAligned(inputs map[string]*MediaPlaylist, cuts []WallRange, origin time.Time) (map[string]*MediaPlaylist, error) {
	if len(inputs) == 0 {
		return nil, fmt.Errorf("m3u8: no input tracks were supplied")
	}
	if len(cuts) == 0 {
		return nil, fmt.Errorf("m3u8: no cuts were supplied")
	}
	names := make([]string, 0, len(inputs))
	for name, p := range inputs {
		if name == "" {
			return nil, fmt.Errorf("m3u8: track names must not be empty")
		}
		if p == nil {
			return nil, fmt.Errorf("m3u8: track %q has no playlist", name)
		}
		names = append(names, name)
	}
	sort.Strings(names)
	if !withinWallClock(origin) {
		return nil, fmt.Errorf("m3u8: origin %s is outside the 1970..2100 range", origin.UTC().Format(time.RFC3339Nano))
	}
	for i, cut := range cuts {
		if !cut.Start.Before(cut.End) {
			return nil, fmt.Errorf("m3u8: cut #%d must be a non-empty half-open interval", i)
		}
		if !withinWallClock(cut.Start) || !withinWallClock(cut.End) {
			return nil, fmt.Errorf("m3u8: cut #%d [%s,%s) lies outside the 1970..2100 range",
				i, cut.Start.UTC().Format(time.RFC3339Nano), cut.End.UTC().Format(time.RFC3339Nano))
		}
	}

	// Reconstruct every clock and resolve every cut before any output
	// playlist is allocated, so failures never produce partial results.
	type trackPlan struct {
		clock  *trackClock
		chains [][]*timedSegment
		total  int64
	}
	plans := make(map[string]*trackPlan, len(names))
	var referenceTotal int64
	for ti, name := range names {
		p := inputs[name]
		if err := rejectUnsupported(name, p); err != nil {
			return nil, err
		}
		clock, err := buildTrackClock(name, p)
		if err != nil {
			return nil, err
		}
		plan := &trackPlan{clock: clock, chains: make([][]*timedSegment, 0, len(cuts))}
		for ci, cut := range cuts {
			chain, err := clock.matchCut(cut, ci)
			if err != nil {
				return nil, err
			}
			var span int64
			for _, ts := range chain {
				span, err = addChecked(span, ts.durationNS)
				if err != nil {
					return nil, fmt.Errorf("track %q cut #%d: %v", name, ci, err)
				}
			}
			if span != cut.End.Sub(cut.Start).Nanoseconds() {
				return nil, fmt.Errorf("track %q cut #%d: selected segment duration %d does not equal cut span %d",
					name, ci, span, cut.End.Sub(cut.Start).Nanoseconds())
			}
			plan.total, err = addChecked(plan.total, span)
			if err != nil {
				return nil, fmt.Errorf("track %q: %v", name, err)
			}
			plan.chains = append(plan.chains, chain)
		}
		if ti == 0 {
			referenceTotal = plan.total
		} else if plan.total != referenceTotal {
			return nil, fmt.Errorf("track %q total duration %dns differs from track %q total %dns",
				name, plan.total, names[0], referenceTotal)
		}
		plans[name] = plan
	}

	outputs := make(map[string]*MediaPlaylist, len(names))
	for _, name := range names {
		plan := plans[name]
		segmentCount := 0
		for _, chain := range plan.chains {
			segmentCount += len(chain)
		}
		out, err := NewMediaPlaylist(0, uint(segmentCount))
		if err != nil {
			return nil, fmt.Errorf("track %q: %v", name, err)
		}
		out.Closed = true
		out.MediaType = VOD
		out.SeqNo = 0
		out.DiscontinuitySeq = 0
		out.TargetDuration = 0

		var accumulated int64
		var previousSourceIndex = -1
		needsByteRange, needsMap := false, false
		for ci, chain := range plan.chains {
			for _, ts := range chain {
				seg := deepCopySegment(ts.segment)
				seg.SeqId = 0
				// First output segment carries no discontinuity; every later
				// jump between non-adjacent source segments, and every source
				// discontinuity, is marked explicitly.
				seg.Discontinuity = previousSourceIndex >= 0 &&
					(ts.segment.Discontinuity || ts.playIndex != previousSourceIndex+1)
				pdt := addNS(origin, accumulated)
				seg.ProgramDateTime = pdt
				if err := out.AppendSegment(seg); err != nil {
					return nil, fmt.Errorf("track %q: %v", name, err)
				}
				if seg.Limit > 0 {
					needsByteRange = true
				}
				if seg.Map != nil {
					needsMap = true
				}
				accumulated, err = addChecked(accumulated, ts.durationNS)
				if err != nil {
					return nil, fmt.Errorf("track %q cut #%d: %v", name, ci, err)
				}
				previousSourceIndex = ts.playIndex
			}
		}
		if needsByteRange && out.ver < 4 {
			out.ver = 4
		}
		if needsMap && out.ver < 5 {
			out.ver = 5
		}
		outputs[name] = out
	}
	return outputs, nil
}
