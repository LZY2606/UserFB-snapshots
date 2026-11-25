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

// Errors reported by TimedBlockReadCloser implementations.
var (
	// ErrTimedReaderUnknownTrack means that a block refers to a track number
	// that is not present in the Tracks element.
	ErrTimedReaderUnknownTrack = errors.New("timed block reader: unknown track number")

	// ErrTimedReaderOverflow means that a timestamp calculation would overflow int64.
	ErrTimedReaderOverflow = errors.New("timed block reader: timestamp overflow")

	// ErrTimedReaderInvalidTimecodeScale means that TimecodeScale is not representable.
	ErrTimedReaderInvalidTimecodeScale = errors.New("timed block reader: invalid TimecodeScale")

	// ErrTimedReaderUnsupportedTrackTimestampScale means that a track has
	// TrackTimestampScale other than the supported value 1.
	ErrTimedReaderUnsupportedTrackTimestampScale = errors.New("timed block reader: TrackTimestampScale other than 1 is not supported")

	// ErrTimedReaderUnsupportedLacing means that a laced block does not have
	// a positive DefaultDuration, so the timing of its frames is undefined.
	ErrTimedReaderUnsupportedLacing = errors.New("timed block reader: laced block requires a positive DefaultDuration")

	// ErrTimedReaderInvalidBlockDuration means that a laced BlockGroup has a
	// BlockDuration that does not match the total duration of its frames.
	ErrTimedReaderInvalidBlockDuration = errors.New("timed block reader: BlockDuration does not match laced frame durations")

	// ErrTimedReaderInvalidDiscardPadding means that DiscardPadding is
	// inconsistent with the duration of the BlockGroup.
	ErrTimedReaderInvalidDiscardPadding = errors.New("timed block reader: invalid DiscardPadding")

	// ErrTimedReaderInvalidBlock means that the Block does not contain any frame.
	ErrTimedReaderInvalidBlock = errors.New("timed block reader: block has no frame")
)

// TimedFrame is a compressed frame read from a Matroska stream together with
// its timing information.
//
// TimestampNS is the presentation timestamp of the compressed frame as stored
// in the container, i.e. the block timestamp corrected by CodecDelay.
// DurationNS is the nominal duration of the frame, or nil when the container
// does not provide enough information to determine it.
//
// PresentStartNS and PresentDurationNS describe the part of the frame that
// remains after applying BlockGroup.DiscardPadding. When DiscardPadding is
// absent (or zero), they describe the whole frame and PresentDurationNS equals
// DurationNS. Completely discarded frames are still returned with a zero
// present duration.
//
// Data always contains the original compressed frame data. It is a copy and
// does not refer to the parser's internal buffers.
type TimedFrame struct {
	Data              []byte
	Keyframe          bool
	TimestampNS       int64
	DurationNS        *int64
	PresentStartNS    int64
	PresentDurationNS *int64
}

// TimedBlockReadCloser is a Matroska timed block reader interface.
type TimedBlockReadCloser interface {
	// ReadFrame reads the next compressed frame of the track.
	//
	// When the underlying stream ends, io.EOF is returned. When an error
	// occurred while parsing the stream or interpreting block timing, that
	// error is returned before io.EOF is reported on the following call.
	ReadFrame() (*TimedFrame, error)

	// Close closes the reader. Frames of the other tracks can still be read
	// after closing this reader. Close is idempotent.
	Close() error

	// TrackEntry returns the TrackEntry of this reader.
	TrackEntry() TrackEntry
}

type timedBlockReader struct {
	f         chan *TimedFrame
	closed    chan struct{}
	closeOnce sync.Once

	mu         sync.Mutex
	userClosed bool
	err        error

	trackEntry TrackEntry
}

func (r *timedBlockReader) TrackEntry() TrackEntry {
	return r.trackEntry
}

func (r *timedBlockReader) Close() error {
	r.mu.Lock()
	r.userClosed = true
	r.mu.Unlock()
	r.closeOnce.Do(func() {
		close(r.closed)
	})
	return nil
}

func (r *timedBlockReader) isClosed() bool {
	r.mu.Lock()
	defer r.mu.Unlock()
	return r.userClosed
}

// signalError stores an error to be reported once unless the reader was
// closed by the caller, and wakes up a blocked ReadFrame.
func (r *timedBlockReader) signalError(err error) {
	r.mu.Lock()
	if !r.userClosed && r.err == nil {
		r.err = err
	}
	r.mu.Unlock()
	r.closeOnce.Do(func() {
		close(r.closed)
	})
}

// finish closes the frame stream after all frames have been delivered.
func (r *timedBlockReader) finish() {
	close(r.f)
}

// takePending returns the pending error and whether the reader was closed by
// the caller. The error is consumed.
func (r *timedBlockReader) takePending() (error, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if r.userClosed {
		return nil, true
	}
	if r.err != nil {
		err := r.err
		r.err = nil
		return err, false
	}
	return nil, false
}

func (r *timedBlockReader) ReadFrame() (*TimedFrame, error) {
	err, closed := r.takePending()
	if closed {
		return nil, io.EOF
	}
	if err != nil {
		return nil, err
	}
	select {
	case f, ok := <-r.f:
		if !ok {
			err, closed := r.takePending()
			switch {
			case closed:
				return nil, io.EOF
			case err != nil:
				return nil, err
			default:
				return nil, io.EOF
			}
		}
		return f, nil
	case <-r.closed:
		return nil, r.terminalError()
	}
}

// terminalError returns the pending error if any, or io.EOF otherwise.
func (r *timedBlockReader) terminalError() error {
	err, closed := r.takePending()
	if closed {
		return io.EOF
	}
	if err != nil {
		return err
	}
	return io.EOF
}

// deliver hands a frame to the reader. It reports false when the reader has
// been closed.
func (r *timedBlockReader) deliver(f *TimedFrame) bool {
	select {
	case r.f <- f:
		return true
	case <-r.closed:
		return false
	}
}

type timedBlockGroup struct {
	Block             ebml.Block
	BlockDuration     uint64
	DiscardPadding    int64
	ReferenceBlock    []int64
	ReferencePriority uint64
}

type timedClusterReader struct {
	Timecode    chan uint64
	SimpleBlock chan ebml.Block
	BlockGroup  chan timedBlockGroup
}

type timedTrack struct {
	entry           TrackEntry
	codecDelay      int64
	defaultDuration int64
	reader          *timedBlockReader
}

// NewTimedBlockReader creates a TimedBlockReadCloser for each TrackEntry in
// the stream. Unlike NewSimpleBlockReader, it keeps the container clock and
// the BlockGroup timing metadata (BlockDuration, DiscardPadding and
// ReferenceBlock) and reports a presentation interval for every compressed
// frame. The compressed frame data is not decoded.
//
// Only TrackTimestampScale == 1 is supported. Laced blocks require a positive
// DefaultDuration and all the frames of a laced block are assumed to have the
// same DefaultDuration.
//
// If the stream contains a format error or a block whose timing cannot be
// expressed, the affected readers report that error from ReadFrame before
// returning io.EOF. A malformed BlockGroup publishes no frames; BlockGroups
// that were already delivered keep their frames.
func NewTimedBlockReader(r io.Reader, opts ...BlockReaderOption) ([]TimedBlockReadCloser, error) {
	// A fatal parse error is reported through the returned readers. Unlike
	// NewSimpleBlockReader the default is not to panic, so that a malformed
	// stream terminates the readers with an error instead of crashing the
	// process. WithOnFatalHandler can still be used to observe the error.
	options := &BlockReaderOptions{}
	for _, o := range opts {
		if err := o.ApplyToBlockReaderOptions(options); err != nil {
			return nil, err
		}
	}

	var header struct {
		Segment struct {
			Info struct {
				TimecodeScale uint64
			} `ebml:"Info"`
			Tracks struct {
				TrackEntry []TrackEntry
			} `ebml:"Tracks,stop"`
		} `ebml:"Segment"`
	}
	switch err := ebml.Unmarshal(r, &header, options.unmarshalOpts...); err {
	case ebml.ErrReadStopped:
	case nil:
		return nil, nil
	default:
		return nil, err
	}

	scale := header.Segment.Info.TimecodeScale
	if scale == 0 {
		scale = 1000000
	}
	if scale > math.MaxInt64 {
		return nil, ErrTimedReaderInvalidTimecodeScale
	}
	scaleNS := int64(scale)

	tracks := make(map[uint64]*timedTrack)
	var readers []*timedBlockReader
	for _, e := range header.Segment.Tracks.TrackEntry {
		if e.TrackTimestampScale != 0 && e.TrackTimestampScale != 1 {
			return nil, ErrTimedReaderUnsupportedTrackTimestampScale
		}
		if e.CodecDelay > math.MaxInt64 || e.DefaultDuration > math.MaxInt64 {
			return nil, ErrTimedReaderOverflow
		}
		t := &timedTrack{
			entry:           e,
			codecDelay:      int64(e.CodecDelay),
			defaultDuration: int64(e.DefaultDuration),
		}
		br := &timedBlockReader{
			f:          make(chan *TimedFrame),
			closed:     make(chan struct{}),
			trackEntry: e,
		}
		t.reader = br
		tracks[e.TrackNumber] = t
		readers = append(readers, br)
	}

	if len(readers) == 0 {
		return nil, nil
	}

	clusterTimecodeCh := make(chan uint64)
	simpleBlockCh := make(chan ebml.Block)
	blockGroupCh := make(chan timedBlockGroup)
	unmarshalErrCh := make(chan error, 1)

	c := struct {
		Cluster timedClusterReader
	}{
		Cluster: timedClusterReader{
			Timecode:    clusterTimecodeCh,
			SimpleBlock: simpleBlockCh,
			BlockGroup:  blockGroupCh,
		},
	}

	go func() {
		var fatal error
		var clusterTimecode uint64

		broadcast := func(err error) {
			if fatal == nil {
				fatal = err
			}
			for _, br := range readers {
				br.signalError(err)
			}
		}

		for clusterTimecodeCh != nil || simpleBlockCh != nil || blockGroupCh != nil || unmarshalErrCh != nil {
			select {
			case tc, ok := <-clusterTimecodeCh:
				if !ok {
					clusterTimecodeCh = nil
					continue
				}
				if fatal == nil {
					clusterTimecode = tc
				}
			case b, ok := <-simpleBlockCh:
				if !ok {
					simpleBlockCh = nil
					continue
				}
				if fatal != nil {
					continue
				}
				if err := dispatchTimedBlock(tracks, scaleNS, clusterTimecode, &b, nil); err != nil {
					broadcast(err)
				}
			case g, ok := <-blockGroupCh:
				if !ok {
					blockGroupCh = nil
					continue
				}
				if fatal != nil {
					continue
				}
				if err := dispatchTimedBlock(tracks, scaleNS, clusterTimecode, &g.Block, &g); err != nil {
					broadcast(err)
				}
			case err, ok := <-unmarshalErrCh:
				if !ok {
					unmarshalErrCh = nil
					continue
				}
				if err != nil {
					broadcast(err)
				}
			}
		}
		for _, br := range readers {
			br.finish()
		}
	}()

	go func() {
		defer func() {
			close(clusterTimecodeCh)
			close(simpleBlockCh)
			close(blockGroupCh)
			close(unmarshalErrCh)
		}()
		if err := ebml.Unmarshal(r, &c, options.unmarshalOpts...); err != nil {
			unmarshalErrCh <- err
			if options.onFatal != nil {
				options.onFatal(err)
			}
		}
	}()

	out := make([]TimedBlockReadCloser, len(readers))
	for i, br := range readers {
		out[i] = br
	}
	return out, nil
}

func dispatchTimedBlock(
	tracks map[uint64]*timedTrack, scale int64, clusterTimecode uint64,
	b *ebml.Block, group *timedBlockGroup,
) error {
	t, ok := tracks[b.TrackNumber]
	if !ok {
		return ErrTimedReaderUnknownTrack
	}
	frames, err := timedFrames(t, scale, clusterTimecode, b, group)
	if err != nil {
		return err
	}
	for _, f := range frames {
		t.reader.deliver(f)
	}
	return nil
}

func timedFrames(
	t *timedTrack, scale int64, clusterTimecode uint64,
	b *ebml.Block, group *timedBlockGroup,
) ([]*TimedFrame, error) {
	if len(b.Data) == 0 {
		return nil, ErrTimedReaderInvalidBlock
	}

	tc, ok := uint64ToInt64(clusterTimecode)
	if !ok {
		return nil, ErrTimedReaderOverflow
	}
	ticks, ok := addInt64(tc, int64(b.Timecode))
	if !ok {
		return nil, ErrTimedReaderOverflow
	}
	nsTicks, ok := mulInt64(ticks, scale)
	if !ok {
		return nil, ErrTimedReaderOverflow
	}
	start, ok := subInt64(nsTicks, t.codecDelay)
	if !ok {
		return nil, ErrTimedReaderOverflow
	}

	keyframe := b.Keyframe
	if group != nil {
		// A BlockGroup frame is a keyframe only when it has no ReferenceBlock.
		// A ReferenceBlock with value 0 is still a dependency.
		keyframe = len(group.ReferenceBlock) == 0
	}

	nFrames := len(b.Data)
	starts := make([]int64, nFrames)
	durations := make([]*int64, nFrames)
	var groupDuration int64

	if b.Lacing != ebml.LacingNo {
		if t.defaultDuration <= 0 {
			return nil, ErrTimedReaderUnsupportedLacing
		}
		d := t.defaultDuration
		starts[0] = start
		for i := 1; i < nFrames; i++ {
			off, ok := mulInt64(int64(i), d)
			if !ok {
				return nil, ErrTimedReaderOverflow
			}
			starts[i], ok = addInt64(start, off)
			if !ok {
				return nil, ErrTimedReaderOverflow
			}
		}
		total, ok := mulInt64(int64(nFrames), d)
		if !ok {
			return nil, ErrTimedReaderOverflow
		}
		groupDuration = total
		for i := range durations {
			dd := d
			durations[i] = &dd
		}
	} else {
		starts[0] = start
		switch {
		case group != nil && group.BlockDuration > 0:
			if group.BlockDuration > math.MaxInt64 {
				return nil, ErrTimedReaderOverflow
			}
			d, ok := mulInt64(int64(group.BlockDuration), scale)
			if !ok {
				return nil, ErrTimedReaderOverflow
			}
			groupDuration = d
		case t.defaultDuration > 0:
			groupDuration = t.defaultDuration
		default:
			groupDuration = -1 // unknown
		}
		if groupDuration >= 0 {
			d := groupDuration
			durations[0] = &d
		}
	}

	if group != nil && b.Lacing != ebml.LacingNo && group.BlockDuration > 0 {
		if group.BlockDuration > math.MaxInt64 {
			return nil, ErrTimedReaderOverflow
		}
		bd, ok := mulInt64(int64(group.BlockDuration), scale)
		if !ok {
			return nil, ErrTimedReaderOverflow
		}
		if bd != groupDuration {
			return nil, ErrTimedReaderInvalidBlockDuration
		}
	}

	padding := int64(0)
	if group != nil {
		padding = group.DiscardPadding
	}

	frames := make([]*TimedFrame, nFrames)
	for i := 0; i < nFrames; i++ {
		f := &TimedFrame{
			Data:        append([]byte(nil), b.Data[i]...),
			Keyframe:    keyframe,
			TimestampNS: starts[i],
		}
		if durations[i] != nil {
			d := *durations[i]
			f.DurationNS = &d
		}
		frames[i] = f
	}

	if padding == 0 {
		for _, f := range frames {
			f.PresentStartNS = f.TimestampNS
			if f.DurationNS != nil {
				d := *f.DurationNS
				f.PresentDurationNS = &d
			}
		}
		return frames, nil
	}

	if groupDuration < 0 {
		// A single frame without a known duration leaves nothing to trim
		// against; any nonzero padding is inconsistent.
		return nil, ErrTimedReaderInvalidDiscardPadding
	}
	pad, ok := absInt64(padding)
	if !ok {
		return nil, ErrTimedReaderOverflow
	}
	if pad > groupDuration {
		return nil, ErrTimedReaderInvalidDiscardPadding
	}

	var keepStart, keepEnd int64
	if padding > 0 {
		keepStart = start
		keepEnd, ok = addInt64(start, groupDuration-padding)
	} else {
		keepStart, ok = addInt64(start, pad)
		keepEnd = groupDuration
		keepEnd, ok = addInt64(start, keepEnd)
	}
	if !ok {
		return nil, ErrTimedReaderOverflow
	}

	for _, f := range frames {
		if f.DurationNS == nil {
			return nil, ErrTimedReaderInvalidDiscardPadding
		}
		end, ok := addInt64(f.TimestampNS, *f.DurationNS)
		if !ok {
			return nil, ErrTimedReaderOverflow
		}
		pStart := clampI64(f.TimestampNS, keepStart, keepEnd)
		pEnd := clampI64(end, keepStart, keepEnd)
		f.PresentStartNS = pStart
		d := pEnd - pStart
		f.PresentDurationNS = &d
	}
	return frames, nil
}

func clampI64(v, lo, hi int64) int64 {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func uint64ToInt64(v uint64) (int64, bool) {
	if v > math.MaxInt64 {
		return 0, false
	}
	return int64(v), true
}

func addInt64(a, b int64) (int64, bool) {
	s := a + b
	if (b > 0 && s < a) || (b < 0 && s > a) {
		return 0, false
	}
	return s, true
}

func subInt64(a, b int64) (int64, bool) {
	s := a - b
	if (b > 0 && s > a) || (b < 0 && s < a) {
		return 0, false
	}
	return s, true
}

func mulInt64(a, b int64) (int64, bool) {
	if a == 0 || b == 0 {
		return 0, true
	}
	if (a == -1 && b == math.MinInt64) || (b == -1 && a == math.MinInt64) {
		return 0, false
	}
	p := a * b
	if p/b != a {
		return 0, false
	}
	return p, true
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
