// Copyright 2017 Hajime Hoshi
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

package mp3

import (
	"errors"
	"io"

	"github.com/hajimehoshi/go-mp3/internal/consts"
	"github.com/hajimehoshi/go-mp3/internal/frame"
)

// seekCheckpointInterval is the number of compressed frames between
// sparse decoder-state checkpoints. A random read re-decodes at most
// this many frames, no matter where the target position is.
const seekCheckpointInterval = 32

// A Decoder is a MP3-decoded stream.
//
// Decoder decodes its underlying source on the fly.
type Decoder struct {
	source      *source
	sampleRate  int
	length      int64
	frameStarts []int64
	// checkpoints[i] restores the cross-frame state (bit reservoir and
	// overlap-add synthesis history) needed to start decoding frame
	// i*seekCheckpointInterval. Entry 0 is nil: the stream starts with
	// empty history. No entry holds decoded PCM.
	checkpoints   []*frame.Checkpoint
	buf           []byte
	frame         *frame.Frame
	pos           int64
	bytesPerFrame int64
}

func (d *Decoder) readFrame() error {
	var err error
	d.frame, _, err = frame.Read(d.source, d.source.pos, d.frame)
	if err != nil {
		if err == io.EOF {
			return io.EOF
		}
		if _, ok := err.(*consts.UnexpectedEOF); ok {
			// TODO: Log here?
			return io.EOF
		}
		return err
	}
	d.buf = append(d.buf, d.frame.Decode()...)
	return nil
}

// Read is io.Reader's Read.
func (d *Decoder) Read(buf []byte) (int, error) {
	if d.length != invalidLength && d.pos >= d.length {
		return 0, io.EOF
	}
	for len(d.buf) == 0 {
		if err := d.readFrame(); err != nil {
			return 0, err
		}
	}
	n := copy(buf, d.buf)
	d.buf = d.buf[n:]
	d.pos += int64(n)
	return n, nil
}

// Seek is io.Seeker's Seek.
//
// Seek returns an error when the underlying source is not io.Seeker.
//
// The offset is measured in the decoder's output bytes (16bit little
// endian, 2 channels, 4 bytes per sample frame) and does not have to be
// aligned to a compressed frame or a sample frame: the bytes returned
// after Seek are exactly what a sequential read would produce at the
// same position. Negative offsets and offsets beyond Length are rejected
// without changing the decoder state.
//
// The first NewDecoder on an io.Seeker source walks the whole stream
// once to build the frame index and sparse state checkpoints. After
// that, each Seek re-decodes no more than
// seekCheckpointInterval compressed frames.
func (d *Decoder) Seek(offset int64, whence int) (int64, error) {
	if d.length == invalidLength {
		// Keep the historical behavior for non-seekable sources.
		if _, ok := d.source.reader.(io.Seeker); !ok {
			return 0, errors.New("mp3: source must be io.Seeker")
		}
		return 0, errors.New("mp3: seek is not available")
	}

	if offset == 0 && whence == io.SeekCurrent {
		// Handle the special case of asking for the current position specially.
		return d.pos, nil
	}

	npos := int64(0)
	switch whence {
	case io.SeekStart:
		npos = offset
	case io.SeekCurrent:
		npos = d.pos + offset
	case io.SeekEnd:
		npos = d.length + offset
	default:
		return 0, errors.New("mp3: invalid whence")
	}
	if npos < 0 {
		return 0, errors.New("mp3: negative position")
	}
	if npos > d.length {
		return 0, errors.New("mp3: position beyond the end")
	}
	if err := d.seekTo(npos); err != nil {
		return 0, err
	}
	return npos, nil
}

// seekTo restores the decoder state so that the next Read yields the
// output bytes starting at npos. It restarts from the nearest checkpoint
// and re-decodes at most seekCheckpointInterval compressed frames.
func (d *Decoder) seekTo(npos int64) error {
	if npos == d.length {
		// Nothing remains: Read returns io.EOF because pos == Length.
		// This also avoids indexing a frame that does not exist when the
		// number of frames is an exact multiple of the checkpoint interval.
		d.frame = nil
		d.buf = nil
		d.pos = npos
		return nil
	}

	targetFrame := npos / d.bytesPerFrame
	restartFrame := targetFrame - targetFrame%seekCheckpointInterval

	d.buf = nil
	var prev *frame.Frame
	if restartFrame > 0 {
		cp := d.checkpoints[restartFrame/seekCheckpointInterval]
		if cp == nil {
			return errors.New("mp3: internal error: missing seek checkpoint")
		}
		prev = cp.NewFrame()
	}
	if _, err := d.source.Seek(d.frameStarts[restartFrame], io.SeekStart); err != nil {
		return err
	}
	d.frame = prev

	// Decode frames [restartFrame, targetFrame] (inclusive). npos is
	// strictly before Length here, so targetFrame is a real frame.
	for i := restartFrame; i < targetFrame; i++ {
		if err := d.readFrame(); err != nil {
			return err
		}
	}
	if err := d.readFrame(); err != nil {
		return err
	}

	skip := (targetFrame-restartFrame)*d.bytesPerFrame + npos%d.bytesPerFrame
	d.buf = d.buf[skip:]
	d.pos = npos
	return nil
}

// SampleRate returns the sample rate like 44100.
//
// Note that the sample rate is retrieved from the first frame.
func (d *Decoder) SampleRate() int {
	return d.sampleRate
}

// prepareIndex walks the whole compressed stream once. It records where
// every compressed frame starts, the total output length, and sparse
// checkpoints of the cross-frame decoder state (bit reservoir window
// and overlap-add history). Decoded PCM is never stored.
func (d *Decoder) prepareIndex() error {
	if d.length != invalidLength {
		return nil
	}
	if _, ok := d.source.reader.(io.Seeker); !ok {
		return nil
	}

	// Keep the current position.
	pos, err := d.source.Seek(0, io.SeekCurrent)
	if err != nil {
		return err
	}
	if err := d.source.rewind(); err != nil {
		return err
	}
	if err := d.source.skipTags(); err != nil {
		return err
	}

	var prev *frame.Frame
	total := int64(0)
	for {
		idx := int64(len(d.frameStarts))
		if idx%seekCheckpointInterval == 0 {
			// Entry 0 is nil: a fresh stream has no history.
			if prev == nil {
				d.checkpoints = append(d.checkpoints, nil)
			} else {
				d.checkpoints = append(d.checkpoints, prev.SaveCheckpoint())
			}
		}

		nf, start, err := frame.Read(d.source, d.source.pos, prev)
		if err != nil {
			if err == io.EOF {
				break
			}
			if _, ok := err.(*consts.UnexpectedEOF); ok {
				// TODO: Log here?
				break
			}
			return err
		}
		// Decode so the overlap-add state (store, v_vec) advances; the
		// PCM output itself is discarded.
		nf.Decode()
		d.frameStarts = append(d.frameStarts, start)
		d.bytesPerFrame = int64(nf.BytesPerFrame())
		total += d.bytesPerFrame
		prev = nf
	}
	d.length = total

	if _, err := d.source.Seek(pos, io.SeekStart); err != nil {
		return err
	}
	return nil
}

const invalidLength = -1

// Length returns the total size in bytes.
//
// Length returns -1 when the total size is not available
// e.g. when the given source is not io.Seeker.
func (d *Decoder) Length() int64 {
	return d.length
}

// NewDecoder decodes the given io.Reader and returns a decoded stream.
//
// The stream is always formatted as 16bit (little endian) 2 channels
// even if the source is single channel MP3.
// Thus, a sample always consists of 4 bytes.
//
// When r implements io.Seeker, NewDecoder reads the whole stream once to
// build the frame index and seek checkpoints; this makes later Seek
// calls exact and cheap. The source itself is not retained in memory.
func NewDecoder(r io.Reader) (*Decoder, error) {
	s := &source{
		reader: r,
	}
	d := &Decoder{
		source: s,
		length: invalidLength,
	}

	if err := s.skipTags(); err != nil {
		return nil, err
	}
	// TODO: Is readFrame here really needed?
	if err := d.readFrame(); err != nil {
		return nil, err
	}
	freq, err := d.frame.SamplingFrequency()
	if err != nil {
		return nil, err
	}
	d.sampleRate = freq

	if err := d.prepareIndex(); err != nil {
		return nil, err
	}
	return d, nil
}
