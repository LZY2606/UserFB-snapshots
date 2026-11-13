package m3u8

/*
 Part of M3U8 parser & generator library.
 This file adds Low-Latency HLS support: EXT-X-PART-INF, EXT-X-PART
 and EXT-X-PRELOAD-HINT tags integrated into the media playlist model.

 Copyright 2013-2024 The Project Developers.
 See the AUTHORS and LICENSE files at the top-level directory of this distribution
 and at https://github.com/grafov/m3u8/
*/

import (
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
)

// Errors returned by the Low-Latency HLS part operations.
var (
	// ErrPartDuration is returned when a part duration is not a
	// positive, finite number.
	ErrPartDuration = errors.New("part duration must be a positive finite number")
	// ErrPartURI is returned when a part or preload hint is created
	// without an URI.
	ErrPartURI = errors.New("part URI must not be empty")
	// ErrPreloadHint is returned when a preload hint cannot be set or
	// cleared in the current playlist state.
	ErrPreloadHint = errors.New("invalid preload hint")
	// ErrPartRange is returned when a BYTERANGE without an offset
	// cannot continue a previous part with the same URI.
	ErrPartRange = errors.New("byterange without offset requires a previous part with the same URI and a known range")
)

// SetPartTargetDuration configures the PART-TARGET value of the
// EXT-X-PART-INF tag. A non-positive value clears the tag. It sets the
// playlist version to 9 as required by Low-Latency HLS.
func (p *MediaPlaylist) SetPartTargetDuration(seconds float64) error {
	if seconds < 0 || math.IsNaN(seconds) || math.IsInf(seconds, 0) {
		return errors.New("part target duration must be a finite non-negative number")
	}
	p.PartTargetDuration = seconds
	version(&p.ver, 9)
	p.buf.Reset()
	return nil
}

// TailParts returns a copy of the parts published for the not yet
// committed media segment. The segment's media sequence number is
// SeqNo plus the number of complete segments in the playlist.
func (p *MediaPlaylist) TailParts() []*Part {
	if len(p.tailParts) == 0 {
		return nil
	}
	out := make([]*Part, len(p.tailParts))
	for i, part := range p.tailParts {
		out[i] = clonePart(part)
	}
	return out
}

// PreloadHint returns a copy of the current PART preload hint or nil
// when no hint is published.
func (p *MediaPlaylist) PreloadHint() *PreloadHint {
	if p.preloadHint == nil {
		return nil
	}
	return cloneHint(p.preloadHint)
}

// AppendPart publishes a new part (EXT-X-PART) at the tail of the
// playlist. The part belongs to the unfinished media segment whose
// sequence number is SeqNo + Count(). The passed part is copied, so
// later modifications by the caller do not affect the playlist.
func (p *MediaPlaylist) AppendPart(part *Part) error {
	if part == nil {
		return ErrPartURI
	}
	if part.URI == "" {
		return ErrPartURI
	}
	if part.Duration <= 0 || math.IsNaN(part.Duration) || math.IsInf(part.Duration, 0) {
		return ErrPartDuration
	}
	next := clonePart(part)
	if next.HasRange && next.Limit <= 0 {
		return ErrPartRange
	}
	if next.HasRange && !next.HasOffset {
		prev, err := p.resolvablePart(next.URI)
		if err != nil {
			return err
		}
		next.Offset = prev.Offset + prev.Limit
		next.HasOffset = true
	}
	p.tailParts = append(p.tailParts, next)
	version(&p.ver, 9)
	p.buf.Reset()
	return nil
}

// SetPreloadHint publishes or replaces the EXT-X-PRELOAD-HINT tag of
// TYPE=PART. The hint is copied and never counts as a completed part.
// Passing nil clears the hint.
func (p *MediaPlaylist) SetPreloadHint(hint *PreloadHint) error {
	if hint == nil {
		p.preloadHint = nil
		p.buf.Reset()
		return nil
	}
	if hint.URI == "" {
		return ErrPreloadHint
	}
	if hint.HasLimit && !hint.HasRange {
		return ErrPreloadHint
	}
	next := cloneHint(hint)
	p.preloadHint = next
	version(&p.ver, 9)
	p.buf.Reset()
	return nil
}

// ClearPreloadHint removes the PART preload hint.
func (p *MediaPlaylist) ClearPreloadHint() {
	p.preloadHint = nil
	p.buf.Reset()
}

// SetTailKey attaches an EXT-X-KEY declaration to the unfinished
// segment. It is written before that segment's parts and moves with
// the segment when it is committed.
func (p *MediaPlaylist) SetTailKey(key *Key) {
	p.tailKey = cloneKey(key)
	p.buf.Reset()
}

// SetTailMap attaches an EXT-X-MAP declaration to the unfinished
// segment. It is written before that segment's parts and moves with
// the segment when it is committed.
func (p *MediaPlaylist) SetTailMap(m *Map) {
	p.tailMap = cloneMap(m)
	p.buf.Reset()
}

// SetTailDiscontinuity marks the unfinished segment with an
// EXT-X-DISCONTINUITY declaration.
func (p *MediaPlaylist) SetTailDiscontinuity() {
	p.tailDiscontinuity = true
	p.buf.Reset()
}

// CommitSegment turns the published tail parts into a complete media
// segment with the given URI and EXTINF duration. The duration is
// supplied by the caller and does not need to match the sum of the
// part durations. On success the tail parts and the preload hint are
// cleared and the next part belongs to the next sequence number. On
// failure (for example when the segment capacity is exhausted) the
// published tail parts and the hint stay untouched.
func (p *MediaPlaylist) CommitSegment(uri string, duration float64, title string) error {
	if p.head == p.tail && p.count > 0 {
		return ErrPlaylistFull
	}
	seg := &MediaSegment{
		SeqId:         p.tailSeqNo(),
		Title:         title,
		URI:           uri,
		Duration:      duration,
		Key:           p.tailKey,
		Map:           p.tailMap,
		Discontinuity: p.tailDiscontinuity,
		Parts:         p.tailParts,
	}
	p.Segments[p.tail] = seg
	p.tail = (p.tail + 1) % p.capacity
	p.count++
	if p.TargetDuration < duration {
		p.TargetDuration = math.Ceil(duration)
	}
	p.tailParts = nil
	p.preloadHint = nil
	p.tailKey = nil
	p.tailMap = nil
	p.tailDiscontinuity = false
	p.buf.Reset()
	return nil
}

// tailSeqNo computes the media sequence number of the unfinished
// segment. It mirrors AppendSegment's SeqId calculation.
func (p *MediaPlaylist) tailSeqNo() uint64 {
	if p.count == 0 {
		return p.SeqNo
	}
	return p.Segments[(p.capacity+p.tail-1)%p.capacity].SeqId + 1
}

// resolvablePart returns the most recent part, within the same media
// sequence number, that can determine the start of a BYTERANGE without
// an explicit offset: it must use the same URI and carry a fully known
// range.
func (p *MediaPlaylist) resolvablePart(uri string) (*Part, error) {
	for i := len(p.tailParts) - 1; i >= 0; i-- {
		prev := p.tailParts[i]
		if prev.URI != uri {
			return nil, ErrPartRange
		}
		if prev.HasRange && prev.HasOffset {
			return prev, nil
		}
	}
	return nil, ErrPartRange
}

func clonePart(part *Part) *Part {
	if part == nil {
		return nil
	}
	copy := *part
	return &copy
}

func cloneHint(hint *PreloadHint) *PreloadHint {
	if hint == nil {
		return nil
	}
	copy := *hint
	return &copy
}

func cloneKey(key *Key) *Key {
	if key == nil {
		return nil
	}
	copy := *key
	return &copy
}

func cloneMap(m *Map) *Map {
	if m == nil {
		return nil
	}
	copy := *m
	return &copy
}

// decodePartLine parses the attribute list of an EXT-X-PART tag and
// appends the part to the unfinished segment.
func (p *MediaPlaylist) decodePartLine(attrs string, strict bool) error {
	part := &Part{}
	for k, v := range decodeParamsLine(attrs) {
		var err error
		switch k {
		case "URI":
			part.URI = v
		case "DURATION":
			if part.Duration, err = strconv.ParseFloat(v, 64); err != nil {
				return fmt.Errorf("part duration parsing error: %s", err)
			}
		case "INDEPENDENT":
			part.Independent = strings.ToUpper(v) == "YES"
		case "GAP":
			part.Gap = strings.ToUpper(v) == "YES"
		case "BYTERANGE":
			part.HasRange = true
			params := strings.SplitN(v, "@", 2)
			if part.Limit, err = strconv.ParseInt(params[0], 10, 64); err != nil {
				return fmt.Errorf("part byterange length parsing error: %s", err)
			}
			if len(params) > 1 {
				part.HasOffset = true
				if part.Offset, err = strconv.ParseInt(params[1], 10, 64); err != nil {
					return fmt.Errorf("part byterange offset parsing error: %s", err)
				}
			}
		}
	}
	if part.URI == "" {
		return ErrPartURI
	}
	if part.Duration <= 0 || math.IsNaN(part.Duration) || math.IsInf(part.Duration, 0) {
		return ErrPartDuration
	}
	if part.HasRange && part.Limit <= 0 {
		return ErrPartRange
	}
	if part.HasRange && !part.HasOffset {
		prev, err := p.resolvablePart(part.URI)
		if err != nil {
			return err
		}
		part.Offset = prev.Offset + prev.Limit
		part.HasOffset = true
	}
	p.tailParts = append(p.tailParts, part)
	return nil
}

// decodePreloadHintLine parses the attribute list of an
// EXT-X-PRELOAD-HINT tag. Only hints of TYPE=PART are modelled; other
// types are ignored.
func (p *MediaPlaylist) decodePreloadHintLine(attrs string, strict bool) error {
	hint := &PreloadHint{}
	for k, v := range decodeParamsLine(attrs) {
		var err error
		switch k {
		case "TYPE":
			if strings.ToUpper(v) != "PART" {
				return nil
			}
		case "URI":
			hint.URI = v
		case "BYTERANGE-START":
			hint.HasRange = true
			if hint.Offset, err = strconv.ParseInt(v, 10, 64); err != nil {
				return fmt.Errorf("preload hint byterange start parsing error: %s", err)
			}
		case "BYTERANGE-LENGTH":
			hint.HasLimit = true
			if hint.Limit, err = strconv.ParseInt(v, 10, 64); err != nil {
				return fmt.Errorf("preload hint byterange length parsing error: %s", err)
			}
		}
	}
	if hint.URI == "" {
		return ErrPreloadHint
	}
	if hint.HasLimit && !hint.HasRange {
		return ErrPreloadHint
	}
	p.preloadHint = hint
	return nil
}
