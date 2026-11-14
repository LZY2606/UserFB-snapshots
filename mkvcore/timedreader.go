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
	"errors"
	"io"
	"math"
	"sync"

	"github.com/at-wat/ebml-go"
)

// ErrLacingWithoutDefaultDuration means that a laced block was found on a
// track without a positive DefaultDuration. The reader never guesses frame
// boundaries from BlockDuration in that case.
var ErrLacingWithoutDefaultDuration = errors.New(
	"laced block on a track without positive DefaultDuration is not supported")

// ErrBlockDurationMismatch means that a laced BlockGroup carries a
// BlockDuration whose nanosecond length does not exactly match the frame count
// times DefaultDuration.
var ErrBlockDurationMismatch = errors.New(
	"BlockDuration does not match frame count times DefaultDuration")

// ErrInvalidDiscardPadding means that a DiscardPadding value is out of range,
// for example its absolute value exceeds the group duration, or it is non-zero
// while the group duration is unknown.
var ErrInvalidDiscardPadding = errors.New("invalid DiscardPadding")

// ErrUnsupportedTrackTimestampScale means that a TrackTimestampScale other
// than 1 was encountered.
var ErrUnsupportedTrackTimestampScale = errors.New(
	"TrackTimestampScale other than 1 is not supported")

// ErrTimestampOverflow means that a timestamp computation overflows int64.
var ErrTimestampOverflow = errors.New("timestamp overflow")

// ErrUnknownTrack means that a block references a track number not declared in
// the Tracks element.
var ErrUnknownTrack = errors.New("block references unknown track number")

// ErrDuplicateTrackNumber means that two TrackEntry elements declare the same
// TrackNumber.
var ErrDuplicateTrackNumber = errors.New("duplicate TrackNumber")

// defaultTimecodeScale is the Matroska default TimecodeScale in nanoseconds.
const defaultTimecodeScale uint64 = 1000000

// timedReaderFrameBuffer bounds how many frames per track may wait for their
// reader before the dispatcher applies backpressure.
const timedReaderFrameBuffer = 1024

// timedInfo is the Segment.Info subset needed to reconstruct the clock.
type timedInfo struct {
	TimecodeScale uint64 `ebml:"TimecodeScale,omitempty"`
}

// timedTrackEntry is the TrackEntry subset needed by the timed reader.
type timedTrackEntry struct {
	TrackNumber         uint64
	TrackUID            uint64
	TrackType           uint8
	FlagEnabled         uint8
	FlagDefault         uint8
	FlagForced          uint8
	FlagLacing          uint8
	MinCache            uint64
	DefaultDuration     uint64
	MaxBlockAdditionID  uint64
	Name                string
	Language            string
	LanguageIETF        string
	CodecID             string
	CodecDecodeAll      uint8
	CodecDelay          uint64
	SeekPreRoll         uint64
	TrackTimestampScale []float64 `ebml:"TrackTimestampScale,omitempty"`
}

func (t timedTrackEntry) toTrackEntry() TrackEntry {
	return TrackEntry{
		TrackNumber:        t.TrackNumber,
		TrackUID:           t.TrackUID,
		TrackType:          t.TrackType,
		FlagEnabled:        t.FlagEnabled,
		FlagDefault:        t.FlagDefault,
		FlagForced:         t.FlagForced,
		FlagLacing:         t.FlagLacing,
		MinCache:           t.MinCache,
		DefaultDuration:    t.DefaultDuration,
		MaxBlockAdditionID: t.MaxBlockAdditionID,
		Name:               t.Name,
		Language:           t.Language,
		LanguageIETF:       t.LanguageIETF,
		CodecID:            t.CodecID,
		CodecDecodeAll:     t.CodecDecodeAll,
		CodecDelay:         t.CodecDelay,
		SeekPreRoll:        t.SeekPreRoll,
	}
}

// timedBlockGroup keeps the BlockGroup metadata that drives the timed reader.
// Scalar pointer fields are not supported by the EBML decoder, so optional
// elements use one-element slices to carry their presence.
type timedBlockGroup struct {
	Block             ebml.Block `ebml:"Block"`
	BlockDuration     []uint64   `ebml:"BlockDuration,omitempty"`
	ReferencePriority uint64     `ebml:"ReferencePriority,omitempty"`
	ReferenceBlock    []int64    `ebml:"ReferenceBlock,omitempty"`
	DiscardPadding    []int64    `ebml:"DiscardPadding,omitempty"`
}

func (g *timedBlockGroup) blockDuration() (uint64, bool) {
	if len(g.BlockDuration) == 0 {
		return 0, false
	}
	return g.BlockDuration[0], true
}

func (g *timedBlockGroup) discardPadding() (int64, bool) {
	if len(g.DiscardPadding) == 0 {
		return 0, false
	}
	return g.DiscardPadding[0], true
}

type timedReadResult struct {
	frame *TimedFrame
	err   error
}

type timedBlockReader struct {
	f          chan timedReadResult
	closed     chan struct{}
	trackEntry TrackEntry
	closeOnce  sync.Once
}

// ReadFrame implements TimedBlockReader.
func (r *timedBlockReader) ReadFrame() (*TimedFrame, error) {
	res, ok := <-r.f
	if !ok {
		return nil, io.EOF
	}
	if res.err != nil {
		return nil, res.err
	}
	return res.frame, nil
}

// Close implements TimedBlockCloser. Closing one track never affects the other
// tracks of the same reader set.
func (r *timedBlockReader) Close() error {
	r.closeOnce.Do(func() {
		close(r.closed)
	})
	return nil
}

// TrackEntry implements TrackEntryGetter.
func (r *timedBlockReader) TrackEntry() TrackEntry {
	return r.trackEntry
}

// NewTimedBlockReader creates a TimedBlockReadCloser for each track declared in
// the Matroska Segment. Unlike NewSimpleBlockReader, it propagates the segment
// clock (TimecodeScale, cluster and block timecodes, CodecDelay), per-frame
// durations and BlockGroup DiscardPadding down to each compressed frame.
//
// Compressed frame data is never decoded. All times are in nanoseconds and may
// be negative before CodecDelay is compensated.
//
// Parse errors, unknown track numbers and clock overflows terminate the
// affected readers with an error instead of being silently turned into EOF.
func NewTimedBlockReader(r io.Reader, opts ...BlockReaderOption) ([]TimedBlockReadCloser, error) {
	options := &BlockReaderOptions{}
	for _, o := range opts {
		if err := o.ApplyToBlockReaderOptions(options); err != nil {
			return nil, err
		}
	}

	var header struct {
		Segment struct {
			Info   timedInfo `ebml:"Info"`
			Tracks struct {
				TrackEntry []timedTrackEntry `ebml:"TrackEntry"`
			} `ebml:"Tracks,stop"`
		} `ebml:"Segment"`
	}
	switch err := ebml.Unmarshal(r, &header, options.unmarshalOpts...); err {
	case ebml.ErrReadStopped:
	default:
		return nil, err
	}

	scale := header.Segment.Info.TimecodeScale
	if scale == 0 {
		scale = defaultTimecodeScale
	}
	if scale > math.MaxInt64 {
		return nil, ErrTimestampOverflow
	}

	var ws []TimedBlockReadCloser
	br := make(map[uint64]*timedBlockReader)
	for _, t := range header.Segment.Tracks.TrackEntry {
		if len(t.TrackTimestampScale) > 0 && t.TrackTimestampScale[0] != 1 {
			return nil, ErrUnsupportedTrackTimestampScale
		}
		if _, dup := br[t.TrackNumber]; dup {
			return nil, ErrDuplicateTrackNumber
		}
		tr := &timedBlockReader{
			// Buffered so one track can be read or closed independently while
			// blocks of the other tracks keep arriving. A closed track is
			// skipped by the dispatcher and never blocks the others.
			f:          make(chan timedReadResult, timedReaderFrameBuffer),
			closed:     make(chan struct{}),
			trackEntry: t.toTrackEntry(),
		}
		ws = append(ws, tr)
		br[t.TrackNumber] = tr
	}

	type clusterReader struct {
		Timecode    chan uint64
		SimpleBlock chan ebml.Block
		BlockGroup  chan timedBlockGroup
	}
	timecodeCh := make(chan uint64)
	simpleBlockCh := make(chan ebml.Block)
	blockGroupCh := make(chan timedBlockGroup)
	parseErrCh := make(chan error, 1)
	c := struct {
		Cluster clusterReader
	}{
		Cluster: clusterReader{
			Timecode:    timecodeCh,
			SimpleBlock: simpleBlockCh,
			BlockGroup:  blockGroupCh,
		},
	}
	go func() {
		err := ebml.Unmarshal(r, &c, options.unmarshalOpts...)
		parseErrCh <- err
		close(timecodeCh)
		close(simpleBlockCh)
		close(blockGroupCh)
	}()

	go func() {
		// Timecode is the first child of its Cluster, so it arrives before the
		// blocks it applies to.
		var timecode uint64
		var fatal error
		setFatal := func(err error) {
			if err != nil && fatal == nil {
				fatal = err
				if options.onFatal != nil {
					options.onFatal(err)
				}
			}
		}

		for timecodeCh != nil || simpleBlockCh != nil || blockGroupCh != nil {
			select {
			case tc, ok := <-timecodeCh:
				if !ok {
					timecodeCh = nil
					continue
				}
				timecode = tc
			case blk, ok := <-simpleBlockCh:
				if !ok {
					simpleBlockCh = nil
					continue
				}
				if fatal != nil {
					continue
				}
				tr, err := trackForBlock(br, blk.TrackNumber)
				if err != nil {
					setFatal(err)
					continue
				}
				frames, err := buildTimedFrames(&blk, blk.Keyframe, nil, timecode, scale, tr.trackEntry)
				if err != nil {
					setFatal(err)
					continue
				}
				setFatal(dispatchTimedFrames(tr, frames))
			case bg, ok := <-blockGroupCh:
				if !ok {
					blockGroupCh = nil
					continue
				}
				if fatal != nil {
					continue
				}
				tr, err := trackForBlock(br, bg.Block.TrackNumber)
				if err != nil {
					setFatal(err)
					continue
				}
				// A BlockGroup is a keyframe only when it has no ReferenceBlock
				// at all; a reference of value 0 still means dependency.
				keyframe := len(bg.ReferenceBlock) == 0
				frames, err := buildTimedFrames(&bg.Block, keyframe, &bg, timecode, scale, tr.trackEntry)
				if err != nil {
					// The whole group is bad: publish none of its frames.
					setFatal(err)
					continue
				}
				setFatal(dispatchTimedFrames(tr, frames))
			case err := <-parseErrCh:
				setFatal(err)
				parseErrCh = nil
			}
		}
		if parseErrCh != nil {
			setFatal(<-parseErrCh)
		}

		// Active readers get the fatal error first, then EOF. Readers closed
		// by their user simply stop here.
		for _, tr := range br {
			if fatal != nil {
				select {
				case tr.f <- timedReadResult{err: fatal}:
				case <-tr.closed:
				}
			}
			close(tr.f)
		}
	}()

	return ws, nil
}

func trackForBlock(br map[uint64]*timedBlockReader, trackNumber uint64) (*timedBlockReader, error) {
	tr, ok := br[trackNumber]
	if !ok {
		return nil, ErrUnknownTrack
	}
	return tr, nil
}

// dispatchTimedFrames sends a fully validated frame group to its track. It
// returns early when the track has already been closed by its user.
func dispatchTimedFrames(tr *timedBlockReader, frames []*TimedFrame) error {
	for _, f := range frames {
		select {
		case tr.f <- timedReadResult{frame: f}:
		case <-tr.closed:
			return nil
		}
	}
	return nil
}

// buildTimedFrames turns one Block/SimpleBlock into timed frames. The group is
// fully parsed and validated before any frame is returned, so a bad group never
// publishes half of its frames. bg is nil for SimpleBlock.
func buildTimedFrames(blk *ebml.Block, keyframe bool, bg *timedBlockGroup,
	clusterTimecode uint64, scale uint64, entry TrackEntry) ([]*TimedFrame, error) {
	nFrames := len(blk.Data)
	if nFrames == 0 {
		return nil, io.ErrUnexpectedEOF
	}
	laced := blk.Lacing != ebml.LacingNo

	if entry.CodecDelay > math.MaxInt64 || entry.DefaultDuration > math.MaxInt64 {
		return nil, ErrTimestampOverflow
	}
	codecDelay := int64(entry.CodecDelay)
	defaultDuration := int64(entry.DefaultDuration)

	// Block start: signed (cluster timecode + block timecode) * scale -
	// CodecDelay. Every step is overflow-checked.
	if clusterTimecode > math.MaxInt64 {
		return nil, ErrTimestampOverflow
	}
	ticks, ok := addInt64(int64(clusterTimecode), int64(blk.Timecode))
	if !ok {
		return nil, ErrTimestampOverflow
	}
	scaled, ok := mulInt64(ticks, int64(scale))
	if !ok {
		return nil, ErrTimestampOverflow
	}
	blockStart, ok := subInt64(scaled, codecDelay)
	if !ok {
		return nil, ErrTimestampOverflow
	}

	var duration int64
	durationKnown := false
	if laced {
		// Laced frames require a positive DefaultDuration; BlockDuration is
		// never used to guess how frames are spaced.
		if defaultDuration <= 0 {
			return nil, ErrLacingWithoutDefaultDuration
		}
		duration = defaultDuration
		durationKnown = true
		if bg != nil {
			if bd, ok := bg.blockDuration(); ok {
				bdNS, ok := blockDurationNS(bd, scale)
				if !ok {
					return nil, ErrTimestampOverflow
				}
				total, ok := mulInt64(int64(nFrames), duration)
				if !ok {
					return nil, ErrTimestampOverflow
				}
				if bdNS != total {
					return nil, ErrBlockDurationMismatch
				}
			}
		}
	} else if bg != nil {
		// Non-laced: BlockDuration (in timecode scale units) wins.
		if bd, ok := bg.blockDuration(); ok {
			d, ok := blockDurationNS(bd, scale)
			if !ok {
				return nil, ErrTimestampOverflow
			}
			duration = d
			durationKnown = true
		} else if defaultDuration > 0 {
			// DefaultDuration is already in nanoseconds; do not scale it.
			duration = defaultDuration
			durationKnown = true
		}
	} else if defaultDuration > 0 {
		// DefaultDuration is already in nanoseconds; do not scale it.
		duration = defaultDuration
		durationKnown = true
	}

	frames := make([]*TimedFrame, nFrames)
	for i := 0; i < nFrames; i++ {
		var ts int64
		if laced {
			offset, ok := mulInt64(int64(i), duration)
			if !ok {
				return nil, ErrTimestampOverflow
			}
			ts, ok = addInt64(blockStart, offset)
			if !ok {
				return nil, ErrTimestampOverflow
			}
		} else {
			ts = blockStart
		}

		// Copy the payload: returned data must never alias a parse buffer that
		// could be reused later.
		data := make([]byte, len(blk.Data[i]))
		copy(data, blk.Data[i])

		f := &TimedFrame{
			Data:           data,
			Keyframe:       keyframe,
			TimestampNS:    ts,
			PresentStartNS: ts,
		}
		if durationKnown {
			d := duration
			f.DurationNS = &d
			pd := duration
			f.PresentDurationNS = &pd
		}
		frames[i] = f
	}

	if bg != nil {
		if padding, ok := bg.discardPadding(); ok {
			if err := applyDiscardPadding(frames, padding, blockStart, durationKnown, duration); err != nil {
				return nil, err
			}
		}
	}
	return frames, nil
}

// applyDiscardPadding clips the nominal per-frame intervals to the retained
// group interval. Padding is applied once to the whole group, not per frame.
func applyDiscardPadding(frames []*TimedFrame, padding int64,
	blockStart int64, durationKnown bool, duration int64) error {
	if !durationKnown {
		// A frame of unknown length can only accept zero padding.
		if padding != 0 {
			return ErrInvalidDiscardPadding
		}
		return nil
	}

	groupDuration, ok := mulInt64(int64(len(frames)), duration)
	if !ok {
		return ErrTimestampOverflow
	}
	absPadding, ok := absInt64(padding)
	if !ok || absPadding > groupDuration {
		return ErrInvalidDiscardPadding
	}
	groupEnd, ok := addInt64(blockStart, groupDuration)
	if !ok {
		return ErrTimestampOverflow
	}

	keptStart, keptEnd := blockStart, groupEnd
	if padding < 0 {
		keptStart, ok = addInt64(blockStart, absPadding)
		if !ok {
			return ErrTimestampOverflow
		}
	}
	if padding > 0 {
		keptEnd, ok = subInt64(groupEnd, absPadding)
		if !ok {
			return ErrTimestampOverflow
		}
	}

	for _, f := range frames {
		frameEnd, ok := addInt64(f.TimestampNS, duration)
		if !ok {
			return ErrTimestampOverflow
		}
		presentStart := clampInt64(f.TimestampNS, keptStart, keptEnd)
		presentEnd := clampInt64(frameEnd, keptStart, keptEnd)
		presentDuration := presentEnd - presentStart
		f.PresentStartNS = presentStart
		f.PresentDurationNS = &presentDuration
	}
	return nil
}

func blockDurationNS(blockDuration uint64, scale uint64) (int64, bool) {
	if blockDuration > math.MaxInt64 {
		return 0, false
	}
	return mulInt64(int64(blockDuration), int64(scale))
}

func clampInt64(v, lo, hi int64) int64 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func addInt64(a, b int64) (int64, bool) {
	if b > 0 && a > math.MaxInt64-b {
		return 0, false
	}
	if b < 0 && a < math.MinInt64-b {
		return 0, false
	}
	return a + b, true
}

func subInt64(a, b int64) (int64, bool) {
	if b > 0 && a < math.MinInt64+b {
		return 0, false
	}
	if b < 0 && a > math.MaxInt64+b {
		return 0, false
	}
	return a - b, true
}

func mulInt64(a, b int64) (int64, bool) {
	if a == 0 || b == 0 {
		return 0, true
	}
	r := a * b
	if r/b != a {
		return 0, false
	}
	return r, true
}

func absInt64(v int64) (int64, bool) {
	if v == math.MinInt64 {
		return 0, false
	}
	if v < 0 {
		return -v, true
	}
	return v, true
}
