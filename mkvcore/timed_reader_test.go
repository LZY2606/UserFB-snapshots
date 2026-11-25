// Copyright 2026 The ebml-go authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

package mkvcore

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math"
	"sync"
	"testing"

	"github.com/at-wat/ebml-go"
	"github.com/at-wat/ebml-go/task_support/taskcheck"
)

// The helpers below construct EBML bytes directly. They are intentionally
// independent of the implementation under test and of the ebml marshaler, so
// that the expected values are not derived from the reader's own output.

var (
	idSegment             = []byte{0x18, 0x53, 0x80, 0x67}
	idInfo                = []byte{0x15, 0x49, 0xA9, 0x66}
	idTimecodeScale       = []byte{0x2A, 0xD7, 0xB1}
	idTracks              = []byte{0x16, 0x54, 0xAE, 0x6B}
	idTrackEntry          = []byte{0xAE}
	idTrackNumber         = []byte{0xD7}
	idCodecDelay          = []byte{0x56, 0xAA}
	idDefaultDuration     = []byte{0x23, 0xE3, 0x83}
	idTrackTimestampScale = []byte{0x23, 0x31, 0x4F}
	idCluster             = []byte{0x1F, 0x43, 0xB6, 0x75}
	idTimecode            = []byte{0xE7}
	idSimpleBlock         = []byte{0xA3}
	idBlockGroup          = []byte{0xA0}
	idBlock               = []byte{0xA1}
	idBlockDuration       = []byte{0x9B}
	idDiscardPadding      = []byte{0x75, 0xA2}
	idReferenceBlock      = []byte{0xFB}
	idReferencePriority   = []byte{0xFA}
)

func vintSize(v uint64, width int) []byte {
	b := make([]byte, width)
	u := v
	for i := width - 1; i >= 0; i-- {
		b[i] = byte(u)
		u >>= 8
	}
	b[0] |= byte(1) << uint(8-width)
	return b
}

// ebmlSize encodes an data size vint, choosing the smallest width.
// The all-ones pattern is reserved for unknown size, so it is skipped.
func ebmlSize(v uint64) []byte {
	for w := 1; w <= 8; w++ {
		if w == 8 {
			return vintSize(v, w)
		}
		max := uint64(1)<<uint(7*w) - 1
		if v < max {
			return vintSize(v, w)
		}
	}
	panic("size out of range")
}

func elem(id, payload []byte) []byte {
	b := append([]byte{}, id...)
	b = append(b, ebmlSize(uint64(len(payload)))...)
	return append(b, payload...)
}

func cat(parts ...[]byte) []byte {
	var out []byte
	for _, p := range parts {
		out = append(out, p...)
	}
	return out
}

func uintBytes(v uint64) []byte {
	n := 1
	for n < 8 && v >= uint64(1)<<uint(8*n) {
		n++
	}
	return vintFixed(v, n)
}

func vintFixed(v uint64, n int) []byte {
	b := make([]byte, n)
	u := v
	for i := n - 1; i >= 0; i-- {
		b[i] = byte(u)
		u >>= 8
	}
	return b
}

func intBytes(v int64) []byte {
	for n := 1; n < 8; n++ {
		shift := uint(8*n - 1)
		if v >= -(int64(1)<<shift) && v <= int64(1)<<shift-1 {
			return vintFixed(uint64(v), n)
		}
	}
	return vintFixed(uint64(v), 8)
}

func uElem(id []byte, v uint64) []byte { return elem(id, uintBytes(v)) }
func sElem(id []byte, v int64) []byte  { return elem(id, intBytes(v)) }

func fElem(id []byte, v float64) []byte {
	b := make([]byte, 8)
	binary.BigEndian.PutUint64(b, math.Float64bits(v))
	return elem(id, b)
}

func signedVint(v int64) []byte {
	switch {
	case v >= -0x3F && v <= 0x3F:
		return vintSize(uint64(v+0x3F), 1)
	case v >= -0x1FFF && v <= 0x1FFF:
		return vintSize(uint64(v+0x1FFF), 2)
	case v >= -0xFFFFF && v <= 0xFFFFF:
		return vintSize(uint64(v+0xFFFFF), 3)
	default:
		panic("lace size delta out of range")
	}
}

func laceHeader(mode ebml.LacingMode, frames [][]byte) []byte {
	switch mode {
	case ebml.LacingNo:
		return nil
	case ebml.LacingFixed:
		return []byte{byte(len(frames) - 1)}
	case ebml.LacingXiph:
		h := []byte{byte(len(frames) - 1)}
		for i := 0; i < len(frames)-1; i++ {
			n := len(frames[i])
			for ; n > 0xFF; n -= 0xFF {
				h = append(h, 0xFF)
			}
			h = append(h, byte(n))
		}
		return h
	case ebml.LacingEBML:
		h := []byte{byte(len(frames) - 1)}
		h = append(h, ebmlSize(uint64(len(frames[0])))...)
		prev := int64(len(frames[0]))
		for i := 1; i < len(frames)-1; i++ {
			h = append(h, signedVint(int64(len(frames[i]))-prev)...)
			prev = int64(len(frames[i]))
		}
		return h
	default:
		panic("unknown lacing")
	}
}

type blockSpec struct {
	track             uint64
	timecode          int16
	keyframe          bool
	lacing            ebml.LacingMode
	frames            [][]byte
	group             bool
	blockDuration     uint64
	hasBlockDuration  bool
	discardPadding    int64
	hasDiscardPadding bool
	referenceBlocks   []int64
	referencePriority uint64
}

func blockPayload(s blockSpec) []byte {
	b := append([]byte{}, ebmlSize(s.track)...)
	b = append(b, byte(uint16(s.timecode)>>8), byte(s.timecode))
	flags := byte(0)
	if s.keyframe {
		flags |= 0x80
	}
	flags |= byte(s.lacing) << 1
	b = append(b, flags)
	b = append(b, laceHeader(s.lacing, s.frames)...)
	for _, f := range s.frames {
		b = append(b, f...)
	}
	return b
}

func buildBlock(s blockSpec) []byte {
	payload := blockPayload(s)
	if !s.group {
		return elem(idSimpleBlock, payload)
	}
	var children []byte
	// The Block is emitted first on purpose: metadata that follows it must
	// still be applied to the frames of the group.
	children = append(children, elem(idBlock, payload)...)
	if s.hasBlockDuration {
		children = append(children, uElem(idBlockDuration, s.blockDuration)...)
	}
	if s.hasDiscardPadding {
		children = append(children, sElem(idDiscardPadding, s.discardPadding)...)
	}
	for _, r := range s.referenceBlocks {
		children = append(children, sElem(idReferenceBlock, r)...)
	}
	if s.referencePriority != 0 {
		children = append(children, uElem(idReferencePriority, s.referencePriority)...)
	}
	return elem(idBlockGroup, children)
}

type trackSpec struct {
	number              uint64
	codecDelay          uint64
	defaultDuration     uint64
	trackTimestampScale *float64
}

type clusterSpec struct {
	timecode uint64
	blocks   []blockSpec
}

type docSpec struct {
	info     *infoSpec
	tracks   []trackSpec
	clusters []clusterSpec
}

type infoSpec struct {
	timecodeScale *uint64
}

func buildDoc(d docSpec) []byte {
	var seg []byte
	if d.info != nil {
		var info []byte
		if d.info.timecodeScale != nil {
			info = append(info, uElem(idTimecodeScale, *d.info.timecodeScale)...)
		}
		seg = append(seg, elem(idInfo, info)...)
	}
	var entries []byte
	for _, tr := range d.tracks {
		var e []byte
		e = append(e, uElem(idTrackNumber, tr.number)...)
		if tr.codecDelay != 0 {
			e = append(e, uElem(idCodecDelay, tr.codecDelay)...)
		}
		if tr.defaultDuration != 0 {
			e = append(e, uElem(idDefaultDuration, tr.defaultDuration)...)
		}
		if tr.trackTimestampScale != nil {
			e = append(e, fElem(idTrackTimestampScale, *tr.trackTimestampScale)...)
		}
		entries = append(entries, elem(idTrackEntry, e)...)
	}
	seg = append(seg, elem(idTracks, entries)...)
	for _, cl := range d.clusters {
		var children []byte
		children = append(children, uElem(idTimecode, cl.timecode)...)
		for _, b := range cl.blocks {
			children = append(children, buildBlock(b)...)
		}
		seg = append(seg, elem(idCluster, children)...)
	}
	return elem(idSegment, seg)
}

func u64p(v uint64) *uint64   { return &v }
func f64p(v float64) *float64 { return &v }
func i64p(v int64) *int64     { return &v }

// frameRec is an independent snapshot of the fields of TimedFrame.
type frameRec struct {
	data       []byte
	keyframe   bool
	ts         int64
	dur        *int64
	present    int64
	presentDur *int64
}

func snapshot(f *TimedFrame) frameRec {
	r := frameRec{
		data:     append([]byte(nil), f.Data...),
		keyframe: f.Keyframe,
		ts:       f.TimestampNS,
		present:  f.PresentStartNS,
	}
	if f.DurationNS != nil {
		v := *f.DurationNS
		r.dur = &v
	}
	if f.PresentDurationNS != nil {
		v := *f.PresentDurationNS
		r.presentDur = &v
	}
	return r
}

func drainAll(r TimedBlockReadCloser) ([]frameRec, error) {
	var out []frameRec
	for {
		f, err := r.ReadFrame()
		if err != nil {
			return out, err
		}
		out = append(out, snapshot(f))
	}
}

func checkFrames(c *taskcheck.C, got, want []frameRec) {
	c.Check(len(got) == len(want), "frame count: got %d, want %d", len(got), len(want))
	n := len(got)
	if len(want) < n {
		n = len(want)
	}
	for i := 0; i < n; i++ {
		g, w := got[i], want[i]
		c.Check(bytes.Equal(g.data, w.data), "frame[%d] data: got % x, want % x", i, g.data, w.data)
		c.Check(g.keyframe == w.keyframe, "frame[%d] keyframe: got %v, want %v", i, g.keyframe, w.keyframe)
		c.Check(g.ts == w.ts, "frame[%d] timestamp: got %d, want %d", i, g.ts, w.ts)
		c.Check(ptrEqual(g.dur, w.dur), "frame[%d] duration: got %s, want %s", i, ptrString(g.dur), ptrString(w.dur))
		c.Check(g.present == w.present, "frame[%d] present start: got %d, want %d", i, g.present, w.present)
		c.Check(ptrEqual(g.presentDur, w.presentDur), "frame[%d] present duration: got %s, want %s", i, ptrString(g.presentDur), ptrString(w.presentDur))
	}
}

func ptrEqual(a, b *int64) bool {
	if a == nil || b == nil {
		return a == b
	}
	return *a == *b
}

func ptrString(p *int64) string {
	if p == nil {
		return "nil"
	}
	return stringifyInt(*p)
}

func stringifyInt(v int64) string {
	if v == 0 {
		return "0"
	}
	neg := v < 0
	var u uint64
	if neg {
		u = uint64(-v)
	} else {
		u = uint64(v)
	}
	var digits []byte
	for u > 0 {
		digits = append([]byte{byte('0' + u%10)}, digits...)
		u /= 10
	}
	if neg {
		return "-" + string(digits)
	}
	return string(digits)
}

func typeFloat64Dummy() float64 { return 0 }

var _ = typeFloat64Dummy

// --- TestTimedLaceClock --------------------------------------------------

func TestTimedLaceClock(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const scale100k = uint64(100000)

		// The example from the specification of the interface:
		// scale=100000, cluster=10, block=-2, CodecDelay=50000,
		// DefaultDuration=200000, three fixed-laced frames.
		promptDoc := func(padding int64) []byte {
			b := blockSpec{
				track:             1,
				timecode:          -2,
				keyframe:          true,
				lacing:            ebml.LacingFixed,
				frames:            [][]byte{{0x11}, {0x12}, {0x13}},
				group:             true,
				hasDiscardPadding: true,
				discardPadding:    padding,
			}
			return buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1, codecDelay: 50000, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 10,
					blocks:   []blockSpec{b},
				}},
			})
		}

		{
			rs, err := NewTimedBlockReader(bytes.NewReader(promptDoc(250000)))
			c.Check(err == nil, "positive padding: NewTimedBlockReader error: %v", err)
			if err == nil {
				c.Check(len(rs) == 1, "positive padding: reader count: got %d", len(rs))
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "positive padding: expected EOF, got %v", derr)
				want := []frameRec{
					{data: []byte{0x11}, keyframe: true, ts: 750000, dur: i64p(200000), present: 750000, presentDur: i64p(200000)},
					{data: []byte{0x12}, keyframe: true, ts: 950000, dur: i64p(200000), present: 950000, presentDur: i64p(150000)},
					{data: []byte{0x13}, keyframe: true, ts: 1150000, dur: i64p(200000), present: 1100000, presentDur: i64p(0)},
				}
				checkFrames(c, got, want)
			}
		}

		{
			rs, err := NewTimedBlockReader(bytes.NewReader(promptDoc(-250000)))
			c.Check(err == nil, "negative padding: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "negative padding: expected EOF, got %v", derr)
				// The group covers [750000,1350000); trimming 250000 from the
				// front keeps [1000000,1350000).
				want := []frameRec{
					{data: []byte{0x11}, keyframe: true, ts: 750000, dur: i64p(200000), present: 1000000, presentDur: i64p(0)},
					{data: []byte{0x12}, keyframe: true, ts: 950000, dur: i64p(200000), present: 1000000, presentDur: i64p(150000)},
					{data: []byte{0x13}, keyframe: true, ts: 1150000, dur: i64p(200000), present: 1150000, presentDur: i64p(200000)},
				}
				checkFrames(c, got, want)
			}
		}

		// Every lacing mode with a non-default TimecodeScale and a
		// DefaultDuration defining per-frame timing.
		laceCases := map[string]struct {
			lacing ebml.LacingMode
			frames [][]byte
		}{
			"No":    {ebml.LacingNo, [][]byte{{0x10}}},
			"Fixed": {ebml.LacingFixed, [][]byte{{0xA0, 0xA1}, {0xB0, 0xB1}, {0xC0, 0xC1}}},
			"Xiph":  {ebml.LacingXiph, [][]byte{{0x01, 0x02, 0x03}, {0x04}, {0x05, 0x06}}},
			"EBML":  {ebml.LacingEBML, [][]byte{{0x21}, {0x22, 0x23, 0x24}, {0x25, 0x26}}},
		}
		for name, lc := range laceCases {
			name, lc := name, lc
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(250000)},
				tracks: []trackSpec{{number: 3, defaultDuration: 300000}},
				clusters: []clusterSpec{{
					timecode: 100,
					blocks: []blockSpec{{
						track: 3, timecode: 2, keyframe: true,
						lacing: lc.lacing, frames: lc.frames,
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "%s lacing: NewTimedBlockReader error: %v", name, err)
			if err != nil {
				continue
			}
			got, derr := drainAll(rs[0])
			c.Check(derr == io.EOF, "%s lacing: expected EOF, got %v", name, derr)
			base := int64(102) * 250000
			var want []frameRec
			for i, f := range lc.frames {
				want = append(want, frameRec{
					data: f, keyframe: true,
					ts: base + int64(i)*300000, dur: i64p(300000),
					present: base + int64(i)*300000, presentDur: i64p(300000),
				})
			}
			checkFrames(c, got, want)
		}

		// Non-laced duration precedence: BlockDuration (in ticks) beats
		// DefaultDuration (already in nanoseconds).
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 7,
					blocks: []blockSpec{{
						track: 1, timecode: 3, group: true,
						hasBlockDuration: true, blockDuration: 7,
						frames: [][]byte{{0xAA}},
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "BlockDuration precedence: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "BlockDuration precedence: expected EOF, got %v", derr)
				want := []frameRec{{
					data: []byte{0xAA}, keyframe: true, // no ReferenceBlock
					ts: 1000000, dur: i64p(700000), present: 1000000, presentDur: i64p(700000),
				}}
				checkFrames(c, got, want)
			}
		}

		// Non-laced DefaultDuration fallback and unknown duration.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1, defaultDuration: 400000}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks:   []blockSpec{{track: 1, timecode: 1, keyframe: true, frames: [][]byte{{0xBB}}}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "DefaultDuration fallback: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "DefaultDuration fallback: expected EOF, got %v", derr)
				want := []frameRec{{data: []byte{0xBB}, keyframe: true, ts: 200000, dur: i64p(400000), present: 200000, presentDur: i64p(400000)}}
				checkFrames(c, got, want)
			}
		}
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks:   []blockSpec{{track: 1, timecode: 1, frames: [][]byte{{0xCC}}}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "unknown duration: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "unknown duration: expected EOF, got %v", derr)
				want := []frameRec{{data: []byte{0xCC}, ts: 200000, present: 200000}}
				checkFrames(c, got, want)
			}
		}

		// Laced block without a positive DefaultDuration is unsupported.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks: []blockSpec{{
						track: 1, lacing: ebml.LacingFixed,
						frames: [][]byte{{0x01}, {0x02}},
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "laced without DefaultDuration: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(len(got) == 0, "laced without DefaultDuration: got %d frames, want 0", len(got))
				c.Check(errors.Is(derr, ErrTimedReaderUnsupportedLacing), "laced without DefaultDuration: got %v, want %v", derr, ErrTimedReaderUnsupportedLacing)
				_, nextErr := rs[0].ReadFrame()
				c.Check(nextErr == io.EOF, "laced without DefaultDuration: error must be followed by EOF, got %v", nextErr)
			}
		}

		// Laced block with a matching BlockDuration is accepted.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale100k)},
				tracks: []trackSpec{{number: 1, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 10,
					blocks: []blockSpec{{
						track: 1, timecode: -2, group: true,
						lacing:           ebml.LacingFixed,
						frames:           [][]byte{{0x01}, {0x02}, {0x03}},
						hasBlockDuration: true, blockDuration: 6, // 600000 == 3*200000
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "matching BlockDuration: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "matching BlockDuration: expected EOF, got %v", derr)
				want := []frameRec{
					{data: []byte{0x01}, keyframe: true, ts: 800000, dur: i64p(200000), present: 800000, presentDur: i64p(200000)},
					{data: []byte{0x02}, keyframe: true, ts: 1000000, dur: i64p(200000), present: 1000000, presentDur: i64p(200000)},
					{data: []byte{0x03}, keyframe: true, ts: 1200000, dur: i64p(200000), present: 1200000, presentDur: i64p(200000)},
				}
				checkFrames(c, got, want)
			}
		}

		// Laced block with a mismatched BlockDuration is an error.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale100k)},
				tracks: []trackSpec{{number: 1, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 10,
					blocks: []blockSpec{{
						track: 1, timecode: 0, group: true,
						lacing:           ebml.LacingFixed,
						frames:           [][]byte{{0x01}, {0x02}, {0x03}},
						hasBlockDuration: true, blockDuration: 5, // 500000 != 600000
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "mismatched BlockDuration: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(len(got) == 0, "mismatched BlockDuration: got %d frames, want 0 (no half group)", len(got))
				c.Check(errors.Is(derr, ErrTimedReaderInvalidBlockDuration), "mismatched BlockDuration: got %v, want %v", derr, ErrTimedReaderInvalidBlockDuration)
				_, nextErr := rs[0].ReadFrame()
				c.Check(nextErr == io.EOF, "mismatched BlockDuration: error must be followed by EOF, got %v", nextErr)
			}
		}

		// CodecDelay may push the start below zero.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale100k)},
				tracks: []trackSpec{{number: 1, codecDelay: 50000}},
				clusters: []clusterSpec{{
					timecode: 0,
					blocks:   []blockSpec{{track: 1, timecode: -10, frames: [][]byte{{0xDD}}}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "negative start: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "negative start: expected EOF, got %v", derr)
				want := []frameRec{{data: []byte{0xDD}, ts: -1050000, present: -1050000}}
				checkFrames(c, got, want)
			}
		}

		// Missing TimecodeScale defaults to 1000000 ns.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{},
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 10,
					blocks:   []blockSpec{{track: 1, timecode: 0, frames: [][]byte{{0xEE}}}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "default scale: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "default scale: expected EOF, got %v", derr)
				want := []frameRec{{data: []byte{0xEE}, ts: 10000000, present: 10000000}}
				checkFrames(c, got, want)
			}
		}

		// Missing Info element also defaults to 1000000 ns.
		{
			doc := buildDoc(docSpec{
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 2,
					blocks:   []blockSpec{{track: 1, timecode: 1, frames: [][]byte{{0xEF}}}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "missing Info: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "missing Info: expected EOF, got %v", derr)
				want := []frameRec{{data: []byte{0xEF}, ts: 3000000, present: 3000000}}
				checkFrames(c, got, want)
			}
		}

		// Overflowing the int64 timestamp space is an error, not a wrapped
		// timestamp.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(1000000)},
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 1 << 63,
					blocks:   []blockSpec{{track: 1, timecode: 0, frames: [][]byte{{0x01}}}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "overflow: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(len(got) == 0, "overflow: got %d frames, want 0", len(got))
				c.Check(errors.Is(derr, ErrTimedReaderOverflow), "overflow: got %v, want %v", derr, ErrTimedReaderOverflow)
			}
		}

		// TrackTimestampScale other than 1 is rejected at construction.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale100k)},
				tracks: []trackSpec{{number: 1, trackTimestampScale: f64p(2)}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks:   []blockSpec{{track: 1, frames: [][]byte{{0x01}}}},
				}},
			})
			_, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(errors.Is(err, ErrTimedReaderUnsupportedTrackTimestampScale), "TrackTimestampScale=2: got %v, want %v", err, ErrTimedReaderUnsupportedTrackTimestampScale)
		}

		// TrackTimestampScale == 1 is accepted.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale100k)},
				tracks: []trackSpec{{number: 1, trackTimestampScale: f64p(1)}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks:   []blockSpec{{track: 1, frames: [][]byte{{0x02}}}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "TrackTimestampScale=1: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "TrackTimestampScale=1: expected EOF, got %v", derr)
				want := []frameRec{{data: []byte{0x02}, ts: 100000, present: 100000}}
				checkFrames(c, got, want)
			}
		}

		// A padding larger than the group duration is rejected, and no
		// partial group is published.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale100k)},
				tracks: []trackSpec{{number: 1, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 0,
					blocks: []blockSpec{{
						track: 1, group: true,
						lacing:            ebml.LacingFixed,
						frames:            [][]byte{{0x01}, {0x02}},
						hasDiscardPadding: true, discardPadding: 500000, // > 2*200000
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "excess padding: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(len(got) == 0, "excess padding: got %d frames, want 0", len(got))
				c.Check(errors.Is(derr, ErrTimedReaderInvalidDiscardPadding), "excess padding: got %v, want %v", derr, ErrTimedReaderInvalidDiscardPadding)
			}
		}

		// A single frame of unknown duration accepts zero padding only.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale100k)},
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks: []blockSpec{{
						track: 1, group: true, frames: [][]byte{{0x01}},
						hasDiscardPadding: true, discardPadding: 1,
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "unknown duration with padding: NewTimedBlockReader error: %v", err)
			if err == nil {
				_, derr := drainAll(rs[0])
				c.Check(errors.Is(derr, ErrTimedReaderInvalidDiscardPadding), "unknown duration with padding: got %v, want %v", derr, ErrTimedReaderInvalidDiscardPadding)
			}
		}
	})
}

// --- TestTimedGroupMetadata ----------------------------------------------

func TestTimedGroupMetadata(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		frame := [][]byte{{0x71, 0x72}}

		// Keyframe of a BlockGroup depends solely on ReferenceBlock.
		// Metadata is placed after the Block in every group.
		cases := map[string]struct {
			refs     []int64
			priority uint64
			wantKey  bool
		}{
			"NoReference":               {nil, 0, true},
			"NoReferenceWithPriority":   {nil, 5, true},
			"ZeroReference":             {[]int64{0}, 0, false},
			"ZeroReferenceWithPriority": {[]int64{0}, 5, false},
			"NonZeroReference":          {[]int64{-1}, 0, false},
		}
		for name, tc := range cases {
			name, tc := name, tc
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks: []blockSpec{{
						track: 1, group: true, frames: frame,
						referenceBlocks:   tc.refs,
						referencePriority: tc.priority,
						// Even when the Block encodes the keyframe flag,
						// the BlockGroup metadata decides.
						keyframe: true,
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "%s: NewTimedBlockReader error: %v", name, err)
			if err != nil {
				continue
			}
			got, derr := drainAll(rs[0])
			c.Check(derr == io.EOF, "%s: expected EOF, got %v", name, derr)
			c.Check(len(got) == 1, "%s: got %d frames, want 1", name, len(got))
			if len(got) != 1 {
				continue
			}
			c.Check(got[0].keyframe == tc.wantKey, "%s: keyframe: got %v, want %v", name, got[0].keyframe, tc.wantKey)
			c.Check(bytes.Equal(got[0].data, frame[0]), "%s: data: got % x, want % x", name, got[0].data, frame[0])
			c.Check(got[0].ts == 100000, "%s: timestamp: got %d, want 100000", name, got[0].ts)
			c.Check(ptrEqual(got[0].dur, i64p(200000)), "%s: duration: got %s, want 200000", name, ptrString(got[0].dur))
		}

		// SimpleBlock keeps the in-block keyframe flag.
		{
			simple := map[string]bool{"key": true, "nonkey": false}
			_ = simple
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks: []blockSpec{
						{track: 1, timecode: 0, keyframe: true, frames: [][]byte{{0x01}}},
						{track: 1, timecode: 5, keyframe: false, frames: [][]byte{{0x02}}},
					},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "SimpleBlock flags: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "SimpleBlock flags: expected EOF, got %v", derr)
				c.Check(len(got) == 2, "SimpleBlock flags: got %d frames, want 2", len(got))
				if len(got) == 2 {
					c.Check(got[0].keyframe, "SimpleBlock flags: frame[0] must be keyframe")
					c.Check(!got[1].keyframe, "SimpleBlock flags: frame[1] must not be keyframe")
				}
			}
		}

		// Metadata after the Block is applied: BlockDuration, DiscardPadding
		// and a ReferenceBlock all follow the Block element.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(100000)},
				tracks: []trackSpec{{number: 1, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 10,
					blocks: []blockSpec{{
						track: 1, timecode: 0, group: true,
						lacing:           ebml.LacingFixed,
						frames:           [][]byte{{0x81, 0x82}, {0x83, 0x84}},
						hasBlockDuration: true, blockDuration: 4, // 400000 = 2*200000
						hasDiscardPadding: true, discardPadding: 100000,
						referenceBlocks: []int64{0},
					}},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "metadata after Block: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(derr == io.EOF, "metadata after Block: expected EOF, got %v", derr)
				want := []frameRec{
					{data: []byte{0x81, 0x82}, keyframe: false, ts: 1000000, dur: i64p(200000), present: 1000000, presentDur: i64p(200000)},
					{data: []byte{0x83, 0x84}, keyframe: false, ts: 1200000, dur: i64p(200000), present: 1200000, presentDur: i64p(100000)},
				}
				checkFrames(c, got, want)
			}
		}

		// Frames are not borrowed from the parser's buffer: later frames must
		// not change previously returned Data.
		{
			var frames [][]byte
			for i := 0; i < 8; i++ {
				frames = append(frames, bytes.Repeat([]byte{byte(0x40 + i)}, 16))
			}
			var blocks []blockSpec
			for i := 0; i < 8; i++ {
				blocks = append(blocks, blockSpec{track: 1, timecode: int16(i), frames: frames[i : i+1]})
			}
			doc := buildDoc(docSpec{
				info:     &infoSpec{timecodeScale: u64p(1000000)},
				tracks:   []trackSpec{{number: 1}},
				clusters: []clusterSpec{{timecode: 0, blocks: blocks}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "data ownership: NewTimedBlockReader error: %v", err)
			if err == nil {
				var got [][]byte
				for {
					f, rerr := rs[0].ReadFrame()
					if rerr == io.EOF {
						break
					}
					c.Check(rerr == nil, "data ownership: ReadFrame error: %v", rerr)
					if rerr != nil {
						break
					}
					got = append(got, f.Data)
				}
				c.Check(len(got) == 8, "data ownership: got %d frames, want 8", len(got))
				for i := 0; i < len(got) && i < len(frames); i++ {
					c.Check(bytes.Equal(got[i], frames[i]), "data ownership: frame[%d] changed after later reads: got % x, want % x", i, got[i], frames[i])
				}
			}
		}
	})
}

// --- TestTimedReaderFlow -------------------------------------------------

// chunkReader returns at most n bytes per Read to exercise fragmented input.
type chunkReader struct {
	r io.Reader
	n int
}

func (c *chunkReader) Read(p []byte) (int, error) {
	if len(p) > c.n {
		p = p[:c.n]
	}
	return c.r.Read(p)
}

func TestTimedReaderFlow(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const scale = uint64(100000)

		build := func() []byte {
			return buildDoc(docSpec{
				info: &infoSpec{timecodeScale: u64p(scale)},
				tracks: []trackSpec{
					{number: 1, codecDelay: 50000, defaultDuration: 200000},
					{number: 2},
				},
				clusters: []clusterSpec{
					{
						timecode: 100,
						blocks: []blockSpec{
							{track: 1, timecode: 0, keyframe: true, frames: [][]byte{{0x11}}},
							{track: 2, timecode: 2, keyframe: false, frames: [][]byte{{0x21}}},
							{
								track: 1, timecode: 5, group: true,
								lacing:            ebml.LacingFixed,
								frames:            [][]byte{{0x12}, {0x13}},
								hasDiscardPadding: true, discardPadding: 150000,
							},
							{track: 1, timecode: 10, frames: [][]byte{{0x14}}},
						},
					},
					{
						timecode: 200,
						blocks: []blockSpec{
							{track: 2, timecode: 0, frames: [][]byte{{0x22}}},
							{track: 1, timecode: -3, frames: [][]byte{{0x15}}},
						},
					},
				},
			})
		}

		// Expected values are computed by hand from scale=100000 and
		// CodecDelay=50000 on track 1.
		want1 := []frameRec{
			{data: []byte{0x11}, keyframe: true, ts: 9950000, dur: i64p(200000), present: 9950000, presentDur: i64p(200000)},
			{data: []byte{0x12}, keyframe: true, ts: 10450000, dur: i64p(200000), present: 10450000, presentDur: i64p(200000)},
			{data: []byte{0x13}, keyframe: true, ts: 10650000, dur: i64p(200000), present: 10650000, presentDur: i64p(50000)},
			{data: []byte{0x14}, keyframe: false, ts: 10950000, dur: i64p(200000), present: 10950000, presentDur: i64p(200000)},
			{data: []byte{0x15}, keyframe: false, ts: 19650000, dur: i64p(200000), present: 19650000, presentDur: i64p(200000)},
		}
		want2 := []frameRec{
			{data: []byte{0x21}, ts: 10200000, present: 10200000},
			{data: []byte{0x22}, ts: 20000000, present: 20000000},
		}

		// Parallel reads over a fragmented reader. Each track must retain its
		// own order and its timestamps must span the clusters correctly.
		{
			rs, err := NewTimedBlockReader(&chunkReader{r: bytes.NewReader(build()), n: 3})
			c.Check(err == nil, "two tracks: NewTimedBlockReader error: %v", err)
			if err == nil {
				c.Check(len(rs) == 2, "two tracks: got %d readers, want 2", len(rs))
				c.Check(rs[0].TrackEntry().TrackNumber == 1, "two tracks: reader[0] track number: got %d, want 1", rs[0].TrackEntry().TrackNumber)
				c.Check(rs[1].TrackEntry().TrackNumber == 2, "two tracks: reader[1] track number: got %d, want 2", rs[1].TrackEntry().TrackNumber)

				var (
					wg   sync.WaitGroup
					got  [2][]frameRec
					errs [2]error
				)
				for i := range rs {
					i := i
					wg.Add(1)
					go func() {
						defer wg.Done()
						got[i], errs[i] = drainAll(rs[i])
					}()
				}
				wg.Wait()
				c.Check(errs[0] == io.EOF, "two tracks: track 1 ended with %v, want EOF", errs[0])
				c.Check(errs[1] == io.EOF, "two tracks: track 2 ended with %v, want EOF", errs[1])
				checkFrames(c, got[0], want1)
				checkFrames(c, got[1], want2)
			}
		}

		// Closing one track must not stop the other one.
		{
			rs, err := NewTimedBlockReader(bytes.NewReader(build()))
			c.Check(err == nil, "close one track: NewTimedBlockReader error: %v", err)
			if err == nil {
				f, rerr := rs[0].ReadFrame()
				c.Check(rerr == nil, "close one track: first ReadFrame error: %v", rerr)
				if rerr == nil {
					c.Check(bytes.Equal(f.Data, []byte{0x11}), "close one track: first frame data: got % x, want 11", f.Data)
				}
				c.Check(rs[0].Close() == nil, "close one track: Close returned an error")
				_, rerr = rs[0].ReadFrame()
				c.Check(rerr == io.EOF, "close one track: closed reader must return EOF, got %v", rerr)

				got, gerr := drainAll(rs[1])
				c.Check(gerr == io.EOF, "close one track: track 2 ended with %v, want EOF", gerr)
				checkFrames(c, got, want2)
			}
		}

		// An unknown track number terminates the active readers with an error
		// followed by EOF instead of a normal end of stream.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale)},
				tracks: []trackSpec{{number: 1}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks: []blockSpec{
						{track: 1, frames: [][]byte{{0xAA}}},
						{track: 99, frames: [][]byte{{0xBB}}},
						{track: 1, timecode: 5, frames: [][]byte{{0xCC}}},
					},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "unknown track: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(len(got) == 1, "unknown track: got %d frames, want 1", len(got))
				if len(got) == 1 {
					c.Check(bytes.Equal(got[0].data, []byte{0xAA}), "unknown track: got data % x, want AA", got[0].data)
				}
				c.Check(errors.Is(derr, ErrTimedReaderUnknownTrack), "unknown track: got %v, want %v", derr, ErrTimedReaderUnknownTrack)
				_, nextErr := rs[0].ReadFrame()
				c.Check(nextErr == io.EOF, "unknown track: error must be followed by EOF, got %v", nextErr)
			}
		}

		// A malformed group publishes no frames at all, while complete groups
		// that were already read are kept.
		{
			doc := buildDoc(docSpec{
				info:   &infoSpec{timecodeScale: u64p(scale)},
				tracks: []trackSpec{{number: 1, defaultDuration: 200000}},
				clusters: []clusterSpec{{
					timecode: 1,
					blocks: []blockSpec{
						{track: 1, frames: [][]byte{{0xAA}}},
						{
							track: 1, group: true,
							lacing:           ebml.LacingFixed,
							frames:           [][]byte{{0x01}, {0x02}, {0x03}},
							hasBlockDuration: true, blockDuration: 5, // mismatch
						},
						{track: 1, timecode: 5, frames: [][]byte{{0xCC}}},
					},
				}},
			})
			rs, err := NewTimedBlockReader(bytes.NewReader(doc))
			c.Check(err == nil, "bad group: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(len(got) == 1, "bad group: got %d frames, want 1 (complete group kept, bad group dropped)", len(got))
				c.Check(errors.Is(derr, ErrTimedReaderInvalidBlockDuration), "bad group: got %v, want %v", derr, ErrTimedReaderInvalidBlockDuration)
			}
		}

		// A truncated stream is reported as an error, not as a clean EOF.
		{
			doc := build()
			truncated := doc[:len(doc)-3]
			rs, err := NewTimedBlockReader(bytes.NewReader(truncated))
			c.Check(err == nil, "truncated: NewTimedBlockReader error: %v", err)
			if err == nil {
				got, derr := drainAll(rs[0])
				c.Check(len(got) > 0, "truncated: no frame was delivered before the error")
				c.Check(derr != nil && derr != io.EOF, "truncated: got %v, want a parse error", derr)
			}
		}
	})
}
