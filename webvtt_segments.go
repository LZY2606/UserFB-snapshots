package astisub

import (
	"errors"
	"fmt"
	"io"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"
)

// MPEG-TS timestamps are 33-bit values cycling at 90kHz.
const (
	webvttSegmentClockModulus = int64(1) << 33
	webvttSegmentClockRate    = int64(90000)
)

// WebVTTSegmentAssembler assembles a sequence of WebVTT segments, as produced
// for HLS playback previews, into a single subtitle track that keeps on being
// editable. Each segment must carry an X-TIMESTAMP-MAP header; the assembler
// unwraps the 33-bit MPEG-TS clock against a 90kHz reference origin and
// re-expresses every cue (and word-level timestamp) relative to that origin.
type WebVTTSegmentAssembler struct {
	origin90k int64

	// Clock state of the current period
	periodIndex   int
	pendingAt     time.Duration
	refUnwrapped  int64
	refMapped     time.Duration
	lastUnwrapped int64
	hasLast       bool

	// Assembled track
	items               []*Item
	regions             map[string]*Region
	regionByFingerprint map[string]*Region
	seen                map[string]*Item
}

// NewWebVTTSegmentAssembler creates an assembler whose output timeline starts
// at the given unwrapped, non negative 90kHz MPEG-TS reference.
func NewWebVTTSegmentAssembler(origin90k int64) (*WebVTTSegmentAssembler, error) {
	if origin90k < 0 {
		return nil, fmt.Errorf("astisub: invalid origin90k %d: must be non negative", origin90k)
	}
	return &WebVTTSegmentAssembler{
		origin90k:           origin90k,
		regions:             make(map[string]*Region),
		regionByFingerprint: make(map[string]*Region),
		seen:                make(map[string]*Item),
	}, nil
}

// StartPeriod declares that the next appended segment starts a new clock
// period: its LOCAL timestamp maps exactly to at, and deduplication identities
// from previous periods are forgotten.
func (a *WebVTTSegmentAssembler) StartPeriod(at time.Duration) error {
	if at < 0 {
		return fmt.Errorf("astisub: invalid period start %s: must be non negative", at)
	}
	a.periodIndex++
	a.pendingAt = at
	a.hasLast = false
	a.seen = make(map[string]*Item)
	return nil
}

// Append parses one WebVTT segment and folds its cues into the assembled
// track. Segments are processed in call order.
func (a *WebVTTSegmentAssembler) Append(r io.Reader) error {
	// Parse the segment
	seg, err := ReadFromWebVTT(r)
	if err != nil {
		return fmt.Errorf("astisub: parsing webvtt segment failed: %w", err)
	}

	// Every segment must carry a valid X-TIMESTAMP-MAP
	if seg.Metadata == nil || seg.Metadata.WebVTTTimestampMap == nil {
		return errors.New("astisub: webvtt segment is missing its X-TIMESTAMP-MAP header")
	}
	tm := seg.Metadata.WebVTTTimestampMap
	if tm.MpegTS < 0 || tm.MpegTS >= webvttSegmentClockModulus {
		return fmt.Errorf("astisub: invalid MPEGTS %d: must be in [0, %d)", tm.MpegTS, webvttSegmentClockModulus)
	}
	if tm.Local < 0 {
		return fmt.Errorf("astisub: invalid LOCAL %s: must be non negative", tm.Local)
	}

	// Unwrap the 33-bit MPEG-TS clock
	var unwrapped int64
	if !a.hasLast {
		if a.periodIndex == 0 {
			// First segment ever: pick the lap closest to the origin
			if unwrapped, err = nearestWebVTTSegmentLap(tm.MpegTS, a.origin90k); err != nil {
				return err
			}
			a.refUnwrapped = a.origin90k
			a.refMapped = 0
		} else {
			// First segment of a new period: LOCAL maps exactly to the
			// declared period start, which becomes the new reference
			unwrapped = tm.MpegTS
			a.refUnwrapped = tm.MpegTS
			a.refMapped = a.pendingAt
		}
	} else {
		// Pick the lap closest to the previous segment's unwrapped value
		if unwrapped, err = nearestWebVTTSegmentLap(tm.MpegTS, a.lastUnwrapped); err != nil {
			return err
		}
	}

	// The segment offset maps a cue's local time onto the output timeline:
	// mapped = refMapped + (unwrapped - refUnwrapped) in nanoseconds + (cue - LOCAL)
	delta, ok := subInt64(unwrapped, a.refUnwrapped)
	if !ok {
		return errors.New("astisub: mpegts clock overflow while unwrapping segment")
	}
	offset, err := mpegtsTicksToNanoseconds(delta)
	if err != nil {
		return err
	}
	if offset, ok = addInt64(offset, int64(a.refMapped)); !ok {
		return errors.New("astisub: time overflow while mapping segment")
	}
	if offset, ok = addInt64(offset, -int64(tm.Local)); !ok {
		return errors.New("astisub: time overflow while mapping segment")
	}

	// Fold cues
	for _, item := range seg.Items {
		if err = a.mapItem(item, offset); err != nil {
			return err
		}
		if item.Region != nil {
			item.Region = a.internRegion(item.Region)
		}
		if item.WebVTTIdentifier != "" {
			key := item.WebVTTIdentifier + "\x1f" + strconv.FormatInt(int64(item.StartAt), 10) + "\x1f" + strconv.FormatInt(int64(item.EndAt), 10)
			if prev, dup := a.seen[key]; dup {
				if webvttSegmentItemFingerprint(prev) != webvttSegmentItemFingerprint(item) {
					return fmt.Errorf("astisub: conflicting contents for cue %q at %s-%s", item.WebVTTIdentifier, item.StartAt, item.EndAt)
				}
				// Exact duplicate within the same period: merge by skipping
				continue
			}
			a.seen[key] = item
		}
		a.items = append(a.items, item)
	}

	a.lastUnwrapped = unwrapped
	a.hasLast = true
	return nil
}

// mapItem shifts an item and its word-level timestamps by the segment offset.
func (a *WebVTTSegmentAssembler) mapItem(item *Item, offset int64) error {
	mapDuration := func(d time.Duration) (time.Duration, error) {
		v, ok := addInt64(offset, int64(d))
		if !ok {
			return 0, fmt.Errorf("astisub: time overflow while mapping cue %q", item.String())
		}
		if v < 0 {
			return 0, fmt.Errorf("astisub: cue %q maps to negative time %s", item.String(), time.Duration(v))
		}
		return time.Duration(v), nil
	}
	var err error
	if item.StartAt, err = mapDuration(item.StartAt); err != nil {
		return err
	}
	if item.EndAt, err = mapDuration(item.EndAt); err != nil {
		return err
	}
	if item.EndAt < item.StartAt {
		return fmt.Errorf("astisub: cue %q ends at %s before it starts at %s", item.String(), item.EndAt, item.StartAt)
	}
	for li := range item.Lines {
		for ii := range item.Lines[li].Items {
			// A zero word timestamp keeps meaning "same as the previous word group"
			if item.Lines[li].Items[ii].StartAt == 0 {
				continue
			}
			if item.Lines[li].Items[ii].StartAt, err = mapDuration(item.Lines[li].Items[ii].StartAt); err != nil {
				return err
			}
		}
	}
	return nil
}

// internRegion returns the output region matching the segment region's
// layout, creating (and if needed renaming) one so that cues from older
// segments keep the layout they were authored with.
func (a *WebVTTSegmentAssembler) internRegion(r *Region) *Region {
	fp := webvttSegmentRegionFingerprint(r)
	if or, ok := a.regionByFingerprint[fp]; ok {
		return or
	}
	name := r.ID
	if _, taken := a.regions[name]; taken {
		for i := 2; ; i++ {
			name = fmt.Sprintf("%s#%d", r.ID, i)
			if _, taken = a.regions[name]; !taken {
				break
			}
		}
	}
	nr := &Region{ID: name, InlineStyle: cloneStyleAttributes(r.InlineStyle)}
	a.regions[name] = nr
	a.regionByFingerprint[fp] = nr
	return nr
}

// Result returns a deep copy of the track assembled so far, ordered by start
// time (ties keep their first-append order). The copy carries no timestamp
// map and shares nothing with the assembler, so it can be edited freely.
func (a *WebVTTSegmentAssembler) Result() *Subtitles {
	o := NewSubtitles()
	regions := make(map[*Region]*Region, len(a.regions))
	for id, r := range a.regions {
		nr := &Region{ID: r.ID, InlineStyle: cloneStyleAttributes(r.InlineStyle)}
		o.Regions[id] = nr
		regions[r] = nr
	}
	o.Items = make([]*Item, 0, len(a.items))
	for _, item := range a.items {
		o.Items = append(o.Items, cloneItem(item, regions))
	}
	sort.SliceStable(o.Items, func(i, j int) bool { return o.Items[i].StartAt < o.Items[j].StartAt })
	return o
}

// nearestWebVTTSegmentLap returns the unwrapped value of m (a raw 33-bit
// MPEG-TS timestamp) on the lap closest to ref. An exact half-turn distance
// is ambiguous and reported as an error.
func nearestWebVTTSegmentLap(m, ref int64) (int64, error) {
	diff, ok := subInt64(ref, m)
	if !ok {
		return 0, errors.New("astisub: mpegts clock overflow while unwrapping segment")
	}
	// Floor division: diff = q*modulus + r with 0 <= r < modulus
	q := diff / webvttSegmentClockModulus
	r := diff % webvttSegmentClockModulus
	if r < 0 {
		q--
		r += webvttSegmentClockModulus
	}
	var lap int64
	switch {
	case r < webvttSegmentClockModulus-r:
		lap = q
	case r > webvttSegmentClockModulus-r:
		lap = q + 1
	default:
		return 0, fmt.Errorf("astisub: ambiguous mpegts wrap: %d is exactly half a turn away from the reference", m)
	}
	lapOffset, ok := mulInt64(lap, webvttSegmentClockModulus)
	if !ok {
		return 0, errors.New("astisub: mpegts clock overflow while unwrapping segment")
	}
	unwrapped, ok := addInt64(m, lapOffset)
	if !ok {
		return 0, errors.New("astisub: mpegts clock overflow while unwrapping segment")
	}
	return unwrapped, nil
}

// mpegtsTicksToNanoseconds converts 90kHz ticks to nanoseconds, rounding down.
func mpegtsTicksToNanoseconds(ticks int64) (int64, error) {
	// ticks * 1e9 / 90000 == ticks * 100000 / 9
	q := ticks / 9
	r := ticks % 9
	if r < 0 {
		q--
		r += 9
	}
	if q > math.MaxInt64/100000 || q < math.MinInt64/100000 {
		return 0, errors.New("astisub: time overflow while converting mpegts ticks")
	}
	return q*100000 + r*100000/9, nil
}

func addInt64(a, b int64) (int64, bool) {
	s := a + b
	if (s > a) != (b > 0) {
		return 0, false
	}
	return s, true
}

func subInt64(a, b int64) (int64, bool) {
	if b == math.MinInt64 {
		return 0, false
	}
	return addInt64(a, -b)
}

func mulInt64(a, b int64) (int64, bool) {
	if a == 0 || b == 0 {
		return 0, true
	}
	p := a * b
	if p/b != a {
		return 0, false
	}
	return p, true
}

// webvttSegmentRegionFingerprint identifies a region's layout.
func webvttSegmentRegionFingerprint(r *Region) string {
	var b strings.Builder
	if r.InlineStyle != nil {
		sa := r.InlineStyle
		fmt.Fprintf(&b, "%s\x1e%d\x1e%s\x1e%s\x1e%s", sa.WebVTTWidth, sa.WebVTTLines, sa.WebVTTRegionAnchor, sa.WebVTTViewportAnchor, sa.WebVTTScroll)
	}
	return b.String()
}

// webvttSegmentItemFingerprint identifies a cue's content and layout.
func webvttSegmentItemFingerprint(item *Item) string {
	var b strings.Builder
	if item.InlineStyle != nil {
		sa := item.InlineStyle
		fmt.Fprintf(&b, "%s\x1e%s\x1e%s\x1e%s\x1e%s\x1e", sa.WebVTTAlign, sa.WebVTTLine, sa.WebVTTPosition.String(), sa.WebVTTSize, sa.WebVTTVertical)
	}
	if item.Region != nil {
		b.WriteString(webvttSegmentRegionFingerprint(item.Region))
	}
	b.WriteString("\x1d")
	for _, l := range item.Lines {
		b.WriteString(l.VoiceName)
		b.WriteString("\x1e")
		for _, li := range l.Items {
			b.WriteString(li.Text)
			b.WriteString("\x1f")
			b.WriteString(strconv.FormatInt(int64(li.StartAt), 10))
			b.WriteString("\x1f")
			if li.InlineStyle != nil {
				for _, t := range li.InlineStyle.WebVTTTags {
					fmt.Fprintf(&b, "%s.%s|%s;", t.Name, strings.Join(t.Classes, "."), t.Annotation)
				}
			}
			b.WriteString("\x1e")
		}
		b.WriteString("\x1d")
	}
	return b.String()
}

// cloneItem deep copies an item, remapping regions through the given table.
func cloneItem(item *Item, regions map[*Region]*Region) *Item {
	o := *item
	if item.Comments != nil {
		o.Comments = append([]string(nil), item.Comments...)
	}
	o.InlineStyle = cloneStyleAttributes(item.InlineStyle)
	if item.Region != nil {
		if r, ok := regions[item.Region]; ok {
			o.Region = r
		}
	}
	if item.Lines != nil {
		o.Lines = make([]Line, len(item.Lines))
		for i, l := range item.Lines {
			nl := l
			if l.Items != nil {
				nl.Items = make([]LineItem, len(l.Items))
				for j, li := range l.Items {
					nli := li
					nli.InlineStyle = cloneStyleAttributes(li.InlineStyle)
					nl.Items[j] = nli
				}
			}
			o.Lines[i] = nl
		}
	}
	return &o
}

// cloneStyleAttributes copies the attributes, deep copying the pointers the
// WebVTT reader and writer can set so that edits never alias.
func cloneStyleAttributes(sa *StyleAttributes) *StyleAttributes {
	if sa == nil {
		return nil
	}
	o := *sa
	if sa.WebVTTPosition != nil {
		p := *sa.WebVTTPosition
		o.WebVTTPosition = &p
	}
	if sa.WebVTTTags != nil {
		o.WebVTTTags = make([]WebVTTTag, len(sa.WebVTTTags))
		for i, t := range sa.WebVTTTags {
			nt := t
			if t.Classes != nil {
				nt.Classes = append([]string(nil), t.Classes...)
			}
			o.WebVTTTags[i] = nt
		}
	}
	if sa.SRTColor != nil {
		c := *sa.SRTColor
		o.SRTColor = &c
	}
	if sa.STLColor != nil {
		c := *sa.STLColor
		o.STLColor = &c
	}
	if sa.TTMLColor != nil {
		c := *sa.TTMLColor
		o.TTMLColor = &c
	}
	if sa.TTMLBackgroundColor != nil {
		c := *sa.TTMLBackgroundColor
		o.TTMLBackgroundColor = &c
	}
	if sa.TTMLTextAlign != nil {
		s := *sa.TTMLTextAlign
		o.TTMLTextAlign = &s
	}
	if sa.TTMLOrigin != nil {
		s := *sa.TTMLOrigin
		o.TTMLOrigin = &s
	}
	if sa.TTMLExtent != nil {
		s := *sa.TTMLExtent
		o.TTMLExtent = &s
	}
	return &o
}
