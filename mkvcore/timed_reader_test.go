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
	"testing"
	"time"

	"github.com/at-wat/ebml-go/task_support/taskcheck"
)

// Minimal standalone EBML byte builder. It deliberately uses only the standard
// library and knows nothing about the reader under test.
var (
	idSegment         = []byte{0x18, 0x53, 0x80, 0x67}
	idInfo            = []byte{0x15, 0x49, 0xA9, 0x66}
	idTimecodeScale   = []byte{0x2A, 0xD7, 0xB1}
	idTracks          = []byte{0x16, 0x54, 0xAE, 0x6B}
	idTrackEntry      = []byte{0xAE}
	idTrackNumber     = []byte{0xD7}
	idDefaultDuration = []byte{0x23, 0xE3, 0x83}
	idCodecDelay      = []byte{0x56, 0xAA}
	idTrackTSScale    = []byte{0x23, 0x31, 0x4F}
	idCluster         = []byte{0x1F, 0x43, 0xB6, 0x75}
	idTimecode        = []byte{0xE7}
	idSimpleBlock     = []byte{0xA3}
	idBlockGroup      = []byte{0xA0}
	idBlock           = []byte{0xA1}
	idBlockDuration   = []byte{0x9B}
	idRefBlock        = []byte{0xFB}
	idDiscardPadding  = []byte{0x75, 0xA2}
	idRefPriority     = []byte{0xFA}
)

// dataSizeVInt encodes n as a minimal EBML VINT data-size value.
func dataSizeVInt(n uint64) []byte {
	for width := 1; width <= 8; width++ {
		if n < (uint64(1)<<(7*width))-1 {
			raw := make([]byte, 8)
			binary.BigEndian.PutUint64(raw, n|(uint64(1)<<(7*width)))
			return raw[8-width:]
		}
	}
	panic("vint too large")
}

func uintPayload(v uint64) []byte {
	if v == 0 {
		return []byte{0}
	}
	n := 0
	for x := v; x > 0; x >>= 8 {
		n++
	}
	out := make([]byte, n)
	for i := 0; i < n; i++ {
		out[i] = byte(v >> (8 * (n - 1 - i)))
	}
	return out
}

func sintPayload(v int64) []byte {
	n := 1
	for {
		lo := int64(-1) << (8*n - 1)
		hi := (int64(1) << (8*n - 1)) - 1
		if v >= lo && v <= hi {
			break
		}
		n++
	}
	out := make([]byte, n)
	u := uint64(v)
	for i := 0; i < n; i++ {
		out[n-1-i] = byte(u >> (8 * i))
	}
	return out
}

func floatPayload(v float64) []byte {
	out := make([]byte, 8)
	binary.BigEndian.PutUint64(out, math.Float64bits(v))
	return out
}

func elem(id, payload []byte) []byte {
	out := make([]byte, 0, len(id)+8+len(payload))
	out = append(out, id...)
	out = append(out, dataSizeVInt(uint64(len(payload)))...)
	out = append(out, payload...)
	return out
}

func uintElem(id []byte, v uint64) []byte {
	return elem(id, uintPayload(v))
}

func sintElem(id []byte, v int64) []byte {
	return elem(id, sintPayload(v))
}

func unknownSizeElem(id, payload []byte) []byte {
	out := make([]byte, 0, len(id)+1+len(payload))
	out = append(out, id...)
	out = append(out, 0xFF)
	out = append(out, payload...)
	return out
}

// Lacing modes as stored in the block flags byte (mode number shifted by 1).
const (
	flagLacingNo    = 0x00
	flagLacingXiph  = 0x02
	flagLacingFixed = 0x04
	flagLacingEBML  = 0x06
)

// buildBlockPayload builds the raw payload of a Block/SimpleBlock element.
func buildBlockPayload(track uint64, tc int16, keyframe bool, lacingFlags byte, frames [][]byte) []byte {
	var p []byte
	p = append(p, dataSizeVInt(track)...)
	p = append(p, byte(tc>>8), byte(tc))
	flags := lacingFlags
	if keyframe {
		flags |= 0x80
	}
	p = append(p, flags)

	switch lacingFlags {
	case flagLacingNo:
		if len(frames) != 1 {
			panic("no-lacing block must hold exactly one frame")
		}
		p = append(p, frames[0]...)
	case flagLacingXiph:
		p = append(p, byte(len(frames)-1))
		for i := 0; i < len(frames)-1; i++ {
			n := len(frames[i])
			for n >= 0xFF {
				p = append(p, 0xFF)
				n -= 0xFF
			}
			p = append(p, byte(n))
		}
		for _, f := range frames {
			p = append(p, f...)
		}
	case flagLacingFixed:
		p = append(p, byte(len(frames)-1))
		for _, f := range frames {
			p = append(p, f...)
		}
	case flagLacingEBML:
		p = append(p, byte(len(frames)-1))
		p = append(p, dataSizeVInt(uint64(len(frames[0])))...)
		prev := len(frames[0])
		for i := 1; i < len(frames)-1; i++ {
			diff := len(frames[i]) - prev
			prev = len(frames[i])
			width := 1
			for {
				bias := (int64(1) << (7*width - 1)) - 1
				u := int64(diff) + bias
				if u >= 0 && u < (int64(1)<<(7*width))-1 {
					raw := make([]byte, 8)
					binary.BigEndian.PutUint64(raw, uint64(u)|(uint64(1)<<(7*width)))
					p = append(p, raw[8-width:]...)
					break
				}
				width++
			}
		}
		for _, f := range frames {
			p = append(p, f...)
		}
	default:
		panic("unknown lacing flags")
	}
	return p
}

func simpleBlockElem(track uint64, tc int16, keyframe bool, lacingFlags byte, frames [][]byte) []byte {
	return elem(idSimpleBlock, buildBlockPayload(track, tc, keyframe, lacingFlags, frames))
}

// blockGroupElem builds a BlockGroup. Children are emitted in the given order
// so tests can place metadata after the Block.
func blockGroupElem(track uint64, tc int16, lacingFlags byte, frames [][]byte, children ...[]byte) []byte {
	var p []byte
	p = append(p, elem(idBlock, buildBlockPayload(track, tc, false, lacingFlags, frames))...)
	for _, c := range children {
		p = append(p, c...)
	}
	return elem(idBlockGroup, p)
}

func trackEntryElem(trackNumber, defaultDuration, codecDelay uint64, extra ...[]byte) []byte {
	var p []byte
	p = append(p, uintElem(idTrackNumber, trackNumber)...)
	if defaultDuration > 0 {
		p = append(p, uintElem(idDefaultDuration, defaultDuration)...)
	}
	if codecDelay > 0 {
		p = append(p, uintElem(idCodecDelay, codecDelay)...)
	}
	for _, e := range extra {
		p = append(p, e...)
	}
	return elem(idTrackEntry, p)
}

// segmentElem builds an unknown-size Segment. scale == 0 omits TimecodeScale so
// the reader applies the 1000000ns default.
func segmentElem(scale uint64, tracks []byte, clusters ...[]byte) []byte {
	var info []byte
	if scale > 0 {
		info = elem(idInfo, uintElem(idTimecodeScale, scale))
	}
	var p []byte
	p = append(p, info...)
	p = append(p, elem(idTracks, tracks)...)
	for _, c := range clusters {
		p = append(p, c...)
	}
	return unknownSizeElem(idSegment, p)
}

func clusterElem(timecode uint64, children ...[]byte) []byte {
	var p []byte
	p = append(p, uintElem(idTimecode, timecode)...)
	for _, c := range children {
		p = append(p, c...)
	}
	return elem(idCluster, p)
}

// choppyReader returns at most one byte per Read to exercise every partial-read
// path of the parser.
type choppyReader struct {
	data []byte
	pos  int
}

func (r *choppyReader) Read(p []byte) (int, error) {
	if len(p) == 0 {
		return 0, nil
	}
	if r.pos >= len(r.data) {
		return 0, io.EOF
	}
	p[0] = r.data[r.pos]
	r.pos++
	return 1, nil
}

func drainTimed(r TimedBlockReadCloser) ([]*TimedFrame, error) {
	var out []*TimedFrame
	for {
		f, err := r.ReadFrame()
		if err != nil {
			return out, err
		}
		out = append(out, f)
	}
}

func drainTimedWithTimeout(r TimedBlockReadCloser) ([]*TimedFrame, error) {
	type result struct {
		frames []*TimedFrame
		err    error
	}
	ch := make(chan result, 1)
	go func() {
		fs, err := drainTimed(r)
		ch <- result{fs, err}
	}()
	select {
	case res := <-ch:
		return res.frames, res.err
	case <-time.After(5 * time.Second):
		return nil, errTimeout
	}
}

func ptrInt64(v int64) *int64 { return &v }

func checkFrame(c *taskcheck.C, got *TimedFrame, data []byte, keyframe bool,
	ts int64, dur *int64, presentStart int64, presentDur *int64, where string) {
	c.Check(bytes.Equal(got.Data, data), "%s: data mismatch, want %v got %v", where, data, got.Data)
	c.Check(got.Keyframe == keyframe, "%s: keyframe want %v got %v", where, keyframe, got.Keyframe)
	c.Check(got.TimestampNS == ts, "%s: timestamp want %d got %d", where, ts, got.TimestampNS)
	if dur == nil {
		c.Check(got.DurationNS == nil, "%s: DurationNS must be nil", where)
	} else {
		c.Check(got.DurationNS != nil && *got.DurationNS == *dur,
			"%s: DurationNS want %d got %v", where, *dur, got.DurationNS)
	}
	c.Check(got.PresentStartNS == presentStart,
		"%s: PresentStartNS want %d got %d", where, presentStart, got.PresentStartNS)
	if presentDur == nil {
		c.Check(got.PresentDurationNS == nil, "%s: PresentDurationNS must be nil", where)
	} else {
		c.Check(got.PresentDurationNS != nil && *got.PresentDurationNS == *presentDur,
			"%s: PresentDurationNS want %d got %v", where, *presentDur, got.PresentDurationNS)
	}
}

// TestTimedLaceClock covers the clock pipeline: non-default scale, signed block
// timecodes, CodecDelay, all four lacing modes, DefaultDuration in nanoseconds
// and cross-frame positive/negative DiscardPadding.
func TestTimedLaceClock(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const (
			scale           = uint64(100000)
			defaultDuration = uint64(200000)
			codecDelay      = uint64(50000)
		)
		// Hand-computed: (10 + tc) * 100000 - 50000, tc == -2 -> 750000.
		wantStarts := []int64{750000, 950000, 1150000}
		frames3 := func(tag byte) [][]byte {
			return [][]byte{{tag, 0x01}, {tag, 0x02}, {tag, 0x03}}
		}

		var blocks []byte
		// No lacing: three separate SimpleBlocks, block timecodes -2, 0, 2.
		for i, tc := range []int16{-2, 0, 2} {
			blocks = append(blocks, simpleBlockElem(1, tc, true, flagLacingNo, [][]byte{{0x10, byte(i + 1)}})...)
		}
		// Xiph, fixed and EBML lacing, each three frames in one block.
		blocks = append(blocks, simpleBlockElem(1, -2, false, flagLacingXiph, frames3(0x20))...)
		blocks = append(blocks, simpleBlockElem(1, -2, false, flagLacingFixed, frames3(0x30))...)
		blocks = append(blocks, simpleBlockElem(1, -2, false, flagLacingEBML,
			[][]byte{{0x40, 0x01, 0x02, 0x03}, {0x40, 0x04, 0x05, 0x06, 0x07, 0x08},
				{0x40, 0x09, 0x0A, 0x0B, 0x0C, 0x0D}})...)
		// Positive padding: 250000ns trimmed off the group end.
		blocks = append(blocks, blockGroupElem(1, -2, flagLacingFixed, frames3(0x50),
			sintElem(idDiscardPadding, 250000))...)
		// Negative padding: 250000ns trimmed off the group start.
		blocks = append(blocks, blockGroupElem(1, -2, flagLacingFixed, frames3(0x60),
			sintElem(idDiscardPadding, -250000))...)

		cluster1 := clusterElem(10, blocks)

		// A second cluster exercising negative timestamps and BlockDuration.
		cluster2 := clusterElem(0,
			simpleBlockElem(1, -1, true, flagLacingNo, [][]byte{{0x70}}),
			blockGroupElem(1, 0, flagLacingNo, [][]byte{{0x71}},
				uintElem(idBlockDuration, 3)),
			// Track 2 has no DefaultDuration: zero padding is accepted and the
			// durations stay unknown.
			blockGroupElem(2, 0, flagLacingNo, [][]byte{{0x72}},
				sintElem(idDiscardPadding, 0)),
		)

		tracks := append(trackEntryElem(1, defaultDuration, codecDelay),
			trackEntryElem(2, 0, 0)...)
		doc := segmentElem(scale, tracks, cluster1, cluster2)

		readers, err := NewTimedBlockReader(bytes.NewReader(doc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		c.Check(len(readers) == 2, "want 2 readers, got %d", len(readers))
		if err != nil || len(readers) != 2 {
			return
		}

		got, err := drainTimedWithTimeout(readers[0])
		c.Check(err == io.EOF, "track 1 must end with EOF, got %v", err)

		// 3 no-lacing frames + 5 laced triplets (xiph, fixed, ebml and the two
		// padding groups) = 18, plus 2 singles in cluster 2 = 20.
		c.Check(len(got) == 20, "want 20 frames on track 1, got %d", len(got))
		if len(got) != 20 {
			return
		}

		dur := ptrInt64(200000)
		// No-lacing triplet: each frame keeps its own block timecode.
		noLacingStarts := []int64{750000, 950000, 1150000}
		for i := 0; i < 3; i++ {
			checkFrame(c, got[i], []byte{0x10, byte(i + 1)}, true,
				noLacingStarts[i], dur, noLacingStarts[i], dur, "no-lacing")
		}
		// Xiph triplet.
		for i := 0; i < 3; i++ {
			checkFrame(c, got[3+i], []byte{0x20, byte(i + 1)}, false,
				wantStarts[i], dur, wantStarts[i], dur, "xiph")
		}
		// Fixed triplet.
		for i := 0; i < 3; i++ {
			checkFrame(c, got[6+i], []byte{0x30, byte(i + 1)}, false,
				wantStarts[i], dur, wantStarts[i], dur, "fixed")
		}
		// EBML triplet.
		ebmlFrames := [][]byte{
			{0x40, 0x01, 0x02, 0x03},
			{0x40, 0x04, 0x05, 0x06, 0x07, 0x08},
			{0x40, 0x09, 0x0A, 0x0B, 0x0C, 0x0D},
		}
		for i := 0; i < 3; i++ {
			checkFrame(c, got[9+i], ebmlFrames[i], false,
				wantStarts[i], dur, wantStarts[i], dur, "ebml-lacing")
		}

		// Positive DiscardPadding, from the worked example in the request:
		// present starts 750000, 950000, 1100000 and durations 200000, 150000, 0.
		posDurs := []int64{200000, 150000, 0}
		posStarts := []int64{750000, 950000, 1100000}
		for i := 0; i < 3; i++ {
			checkFrame(c, got[12+i], []byte{0x50, byte(i + 1)}, true,
				wantStarts[i], dur, posStarts[i], ptrInt64(posDurs[i]), "positive-padding")
		}

		// Negative padding trims the group front: retained [1000000, 1350000).
		negStarts := []int64{1000000, 1000000, 1150000}
		negDurs := []int64{0, 150000, 200000}
		for i := 0; i < 3; i++ {
			checkFrame(c, got[15+i], []byte{0x60, byte(i + 1)}, true,
				wantStarts[i], dur, negStarts[i], ptrInt64(negDurs[i]), "negative-padding")
		}

		// Cluster 2, negative timestamp: (0 - 1) * 100000 - 50000 = -150000.
		checkFrame(c, got[18], []byte{0x70}, true, -150000, dur, -150000, dur, "negative-ts")
		// BlockDuration=3 ticks -> 300000ns; ts = 0 * 100000 - 50000 = -50000.
		checkFrame(c, got[19], []byte{0x71}, true, -50000, ptrInt64(300000),
			-50000, ptrInt64(300000), "block-duration")

		got2, err2 := drainTimedWithTimeout(readers[1])
		c.Check(err2 == io.EOF, "track 2 must end with EOF, got %v", err2)
		c.Check(len(got2) == 1, "want 1 frame on track 2, got %d", len(got2))
		if len(got2) == 1 {
			// (0 + 0) * 100000 - 0 = 0; durations unknown, zero padding.
			checkFrame(c, got2[0], []byte{0x72}, true, 0, nil, 0, nil, "unknown-duration")
		}
	})
}

// TestTimedGroupMetadata covers BlockGroup keyframe rules, metadata placed
// after Block, BlockDuration/lacing consistency and DiscardPadding validation.
func TestTimedGroupMetadata(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const scale = uint64(100000)
		tracks := append(trackEntryElem(1, 200000, 0), trackEntryElem(2, 0, 0)...)

		// ReferenceBlock of value 0 still means dependency. Metadata is
		// intentionally emitted after Block.
		refZero := blockGroupElem(1, 0, flagLacingNo, [][]byte{{0x01}},
			sintElem(idRefBlock, 0))
		// ReferencePriority alone does not make a BlockGroup a keyframe.
		refPrioOnly := blockGroupElem(1, 1, flagLacingNo, [][]byte{{0x02}},
			uintElem(idRefPriority, 1))
		// Two references, one negative.
		refTwo := blockGroupElem(1, 2, flagLacingNo, [][]byte{{0x03}},
			sintElem(idRefBlock, 0), sintElem(idRefBlock, -123))
		// SimpleBlock keyframe flag comes from the block header.
		simpleKey := simpleBlockElem(1, 3, true, flagLacingNo, [][]byte{{0x04}})
		simpleNonKey := simpleBlockElem(1, 4, false, flagLacingNo, [][]byte{{0x05}})
		// Laced group with BlockDuration exactly matching 3 * 200000ns:
		// 600000ns / 100000ns per tick = 6 ticks.
		lacedGood := blockGroupElem(1, 5, flagLacingFixed,
			[][]byte{{0x06, 0x01}, {0x06, 0x02}, {0x06, 0x03}},
			uintElem(idBlockDuration, 6))

		doc := segmentElem(scale, tracks, clusterElem(0,
			refZero, refPrioOnly, refTwo, simpleKey, simpleNonKey, lacedGood))
		readers, err := NewTimedBlockReader(bytes.NewReader(doc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		c.Check(len(readers) == 2, "want 2 readers, got %d", len(readers))
		if err != nil || len(readers) != 2 {
			return
		}
		got, err := drainTimedWithTimeout(readers[0])
		c.Check(err == io.EOF, "want EOF, got %v", err)
		c.Check(len(got) == 8, "want 8 frames (3 singles + 2 simple + 3 laced), got %d", len(got))
		if len(got) == 8 {
			c.Check(got[0].Keyframe == false, "ReferenceBlock=0 must not be a keyframe")
			c.Check(got[1].Keyframe == true, "no ReferenceBlock means keyframe")
			c.Check(got[2].Keyframe == false, "any ReferenceBlock removes keyframe flag")
			c.Check(got[3].Keyframe == true, "SimpleBlock keyframe flag")
			c.Check(got[4].Keyframe == false, "SimpleBlock non-keyframe flag")
			for i := 5; i < 8; i++ {
				c.Check(got[i].DurationNS != nil && *got[i].DurationNS == 200000,
					"laced frame %d duration", i)
			}
			// Laced BlockDuration was consistent: timestamps stay nominal.
			c.Check(got[5].TimestampNS == 5*100000, "laced group start")
			c.Check(got[7].TimestampNS == 5*100000+2*200000, "laced group last frame")
		}

		// Mismatching BlockDuration on a laced group: the whole group is bad
		// and none of its frames may be published.
		badDoc := segmentElem(0, trackEntryElem(1, 200000, 0), clusterElem(0,
			simpleBlockElem(1, 0, true, flagLacingNo, [][]byte{{0x10}}),
			blockGroupElem(1, 1, flagLacingFixed,
				[][]byte{{0x11}, {0x12}, {0x13}},
				uintElem(idBlockDuration, 599)),
			simpleBlockElem(1, 2, true, flagLacingNo, [][]byte{{0x14}}),
		))
		badReaders, err := NewTimedBlockReader(bytes.NewReader(badDoc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		if err == nil && len(badReaders) == 1 {
			badGot, badErr := drainTimedWithTimeout(badReaders[0])
			c.Check(len(badGot) == 1, "only the frame before the bad group is published, got %d", len(badGot))
			c.Check(bytes.Equal(badGot[0].Data, []byte{0x10}), "surviving frame payload")
			c.Check(errors.Is(badErr, ErrBlockDurationMismatch),
				"want ErrBlockDurationMismatch, got %v", badErr)
			// The error is delivered once; further reads end with EOF.
			_, nextErr := badReaders[0].ReadFrame()
			c.Check(nextErr == io.EOF, "want EOF after fatal error, got %v", nextErr)
		}

		// Non-zero padding on a single frame of unknown duration.
		padDoc := segmentElem(0, trackEntryElem(2, 0, 0), clusterElem(0,
			blockGroupElem(2, 0, flagLacingNo, [][]byte{{0x20}},
				sintElem(idDiscardPadding, 100)),
		))
		padReaders, err := NewTimedBlockReader(bytes.NewReader(padDoc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		if err == nil && len(padReaders) == 1 {
			padGot, padErr := drainTimedWithTimeout(padReaders[0])
			c.Check(len(padGot) == 0, "bad padding group publishes no frames, got %d", len(padGot))
			c.Check(errors.Is(padErr, ErrInvalidDiscardPadding),
				"want ErrInvalidDiscardPadding, got %v", padErr)
		}

		// Padding larger than the group duration.
		overPadDoc := segmentElem(0, trackEntryElem(1, 200000, 0), clusterElem(0,
			blockGroupElem(1, 0, flagLacingNo, [][]byte{{0x30}},
				uintElem(idBlockDuration, 2), sintElem(idDiscardPadding, 3000000)),
		))
		overReaders, err := NewTimedBlockReader(bytes.NewReader(overPadDoc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		if err == nil && len(overReaders) == 1 {
			_, overErr := drainTimedWithTimeout(overReaders[0])
			c.Check(errors.Is(overErr, ErrInvalidDiscardPadding),
				"want ErrInvalidDiscardPadding, got %v", overErr)
		}

		// Lacing without positive DefaultDuration is explicitly rejected.
		noDurDoc := segmentElem(0, trackEntryElem(2, 0, 0), clusterElem(0,
			blockGroupElem(2, 0, flagLacingFixed, [][]byte{{0x40}, {0x41}}),
		))
		noDurReaders, err := NewTimedBlockReader(bytes.NewReader(noDurDoc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		if err == nil && len(noDurReaders) == 1 {
			_, noDurErr := drainTimedWithTimeout(noDurReaders[0])
			c.Check(errors.Is(noDurErr, ErrLacingWithoutDefaultDuration),
				"want ErrLacingWithoutDefaultDuration, got %v", noDurErr)
		}

		// TrackTimestampScale other than 1 is rejected at reader creation.
		ttsDoc := segmentElem(0, trackEntryElem(1, 200000, 0,
			elem(idTrackTSScale, floatPayload(1.5))), clusterElem(0))
		_, err = NewTimedBlockReader(bytes.NewReader(ttsDoc))
		c.Check(errors.Is(err, ErrUnsupportedTrackTimestampScale),
			"want ErrUnsupportedTrackTimestampScale, got %v", err)
	})
}

// TestTimedReaderFlow covers multi-track ordering through a choppy reader,
// closing one track while the other continues, error termination and payload
// ownership.
func TestTimedReaderFlow(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const scale = uint64(100000)
		tracks := append(trackEntryElem(1, 200000, 50000), trackEntryElem(2, 200000, 50000)...)

		cluster1 := clusterElem(10,
			simpleBlockElem(1, -2, true, flagLacingNo, [][]byte{{0xA1}}),
			simpleBlockElem(2, -2, true, flagLacingNo, [][]byte{{0xB1}}),
			simpleBlockElem(1, 0, false, flagLacingNo, [][]byte{{0xA2}}),
			simpleBlockElem(2, 0, false, flagLacingNo, [][]byte{{0xB2}}),
		)
		cluster2 := clusterElem(20,
			simpleBlockElem(1, -2, true, flagLacingNo, [][]byte{{0xA3}}),
			simpleBlockElem(2, -2, true, flagLacingNo, [][]byte{{0xB3}}),
		)
		doc := segmentElem(scale, tracks, cluster1, cluster2)

		// One byte per Read.
		readers, err := NewTimedBlockReader(&choppyReader{data: doc})
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		c.Check(len(readers) == 2, "want 2 readers, got %d", len(readers))
		if err != nil || len(readers) != 2 {
			return
		}
		c.Check(readers[0].TrackEntry().TrackNumber == 1, "reader 0 track number")
		c.Check(readers[0].TrackEntry().CodecDelay == 50000, "CodecDelay propagated")
		c.Check(readers[0].TrackEntry().DefaultDuration == 200000, "DefaultDuration propagated")

		// Hand-computed expectations, not derived from the reader output.
		want1 := []struct {
			data []byte
			ts   int64
		}{
			{data: []byte{0xA1}, ts: 750000},
			{data: []byte{0xA2}, ts: 950000},
			{data: []byte{0xA3}, ts: 1750000},
		}
		want2 := []struct {
			data []byte
			ts   int64
		}{
			{data: []byte{0xB1}, ts: 750000},
			{data: []byte{0xB2}, ts: 950000},
			{data: []byte{0xB3}, ts: 1750000},
		}

		// Read the first two frames of track 1, then close it. Track 2 must
		// keep streaming to EOF unaffected.
		for i := 0; i < 2; i++ {
			f, ferr := readers[0].ReadFrame()
			c.Check(ferr == nil, "track 1 frame %d read: %v", i, ferr)
			if ferr == nil {
				c.Check(bytes.Equal(f.Data, want1[i].data) && f.TimestampNS == want1[i].ts,
					"track 1 frame %d", i)
			}
		}
		c.Check(readers[0].Close() == nil, "Close track 1")

		got2, err2 := drainTimedWithTimeout(readers[1])
		c.Check(err2 == io.EOF, "track 2 ends with EOF, got %v", err2)
		c.Check(len(got2) == 3, "track 2 frame count, got %d", len(got2))
		if len(got2) == 3 {
			for i, w := range want2 {
				c.Check(bytes.Equal(got2[i].Data, w.data) && got2[i].TimestampNS == w.ts,
					"track 2 frame %d", i)
				c.Check(got2[i].DurationNS != nil && *got2[i].DurationNS == 200000,
					"track 2 frame %d duration", i)
			}
		}
		// A closed reader eventually reaches EOF without hanging.
		_, errClosed := drainTimedWithTimeout(readers[0])
		c.Check(errClosed == io.EOF, "closed track 1 ends with EOF, got %v", errClosed)

		// Unknown track number: active readers must receive a real error first.
		unknownDoc := segmentElem(scale, trackEntryElem(1, 200000, 50000), clusterElem(10,
			simpleBlockElem(9, 0, true, flagLacingNo, [][]byte{{0xC1}}),
		))
		unknownReaders, err := NewTimedBlockReader(bytes.NewReader(unknownDoc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		if err == nil && len(unknownReaders) == 1 {
			_, unknownErr := drainTimedWithTimeout(unknownReaders[0])
			c.Check(errors.Is(unknownErr, ErrUnknownTrack),
				"want ErrUnknownTrack, got %v", unknownErr)
			_, eofErr := unknownReaders[0].ReadFrame()
			c.Check(eofErr == io.EOF, "EOF after unknown-track error, got %v", eofErr)
		}

		// Clock overflow mid-stream.
		overflowDoc := segmentElem(uint64(1)<<40, trackEntryElem(1, 200000, 0), clusterElem(
			uint64(1)<<30,
			simpleBlockElem(1, 0, true, flagLacingNo, [][]byte{{0xD1}}),
		))
		overflowReaders, err := NewTimedBlockReader(bytes.NewReader(overflowDoc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		if err == nil && len(overflowReaders) == 1 {
			_, overflowErr := drainTimedWithTimeout(overflowReaders[0])
			c.Check(errors.Is(overflowErr, ErrTimestampOverflow),
				"want ErrTimestampOverflow, got %v", overflowErr)
		}

		// Returned payloads are owned by the caller and do not alias a buffer
		// reused by the parser.
		aliasDoc := segmentElem(scale, trackEntryElem(1, 0, 0), clusterElem(10,
			simpleBlockElem(1, 0, true, flagLacingNo, [][]byte{{0xE1, 0xE2}}),
			simpleBlockElem(1, 1, true, flagLacingNo, [][]byte{{0xE1, 0xE2}}),
		))
		aliasReaders, err := NewTimedBlockReader(bytes.NewReader(aliasDoc))
		c.Check(err == nil, "NewTimedBlockReader error: %v", err)
		if err == nil && len(aliasReaders) == 1 {
			f1, e1 := aliasReaders[0].ReadFrame()
			c.Check(e1 == nil, "first alias frame: %v", e1)
			if e1 == nil {
				f1.Data[0] = 0xFF
				f2, e2 := aliasReaders[0].ReadFrame()
				c.Check(e2 == nil, "second alias frame: %v", e2)
				c.Check(bytes.Equal(f2.Data, []byte{0xE1, 0xE2}),
					"later frame must not be affected by mutating an earlier one, got %v", f2.Data)
			}
		}
	})
}
