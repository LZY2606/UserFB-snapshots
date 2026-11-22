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
	"encoding/binary"
	"errors"
	"io"

	"github.com/hajimehoshi/go-mp3/internal/consts"
	"github.com/hajimehoshi/go-mp3/internal/frame"
	"github.com/hajimehoshi/go-mp3/internal/frameheader"
	"github.com/hajimehoshi/go-mp3/internal/framelock"
)

// A Decoder is a MP3-decoded stream.
//
// Decoder decodes its underlying source on the fly.
type Decoder struct {
	source        *source
	sampleRate    int
	length        int64
	frameStarts   []int64
	buf           []byte
	frame         *frame.Frame
	pos           int64
	bytesPerFrame int64
	// freeLock is set for free-bitrate streams and supplies the frame
	// length, which free-format headers do not carry. It is nil for streams
	// whose bitrate is listed in the bitrate table. Each Decoder keeps its
	// own lock state.
	freeLock *framelock.Lock
}

func (d *Decoder) readFrame() error {
	var err error
	d.frame, _, err = frame.Read(d.source, d.source.pos, d.frame, d.freeLock)
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

// lockFreeFormat detects a free-bitrate (bitrate index 0) stream and locks
// its frame length from three consecutive compatible frame headers. Every
// byte consumed by the look-ahead is returned to the source in its original
// order, so the following frame reads see the complete stream again.
//
// Only streams whose first header already uses the free bitrate index are
// scanned; regular streams take the table-driven path without any extra
// reading.
func (d *Decoder) lockFreeFormat() error {
	peek := make([]byte, 4)
	n, _ := d.source.ReadFull(peek)
	if n < 4 {
		// Let the regular frame reader report the short header.
		if n > 0 {
			d.source.Unread(peek[:n])
		}
		return nil
	}
	h := frameheader.FrameHeader(binary.BigEndian.Uint32(peek))
	if !h.IsValid() || !h.IsFreeBitrate() ||
		h.ID() == consts.Version2_5 || h.Layer() != consts.Layer3 {
		d.source.Unread(peek)
		return nil
	}

	// The peeked bytes seed the finder scan, so the first header counts
	// towards the three required headers. They are returned exactly once,
	// together with the rest of the look-ahead.
	seedPos := d.source.pos - int64(n)
	finder := framelock.NewFinder(d.source, seedPos, peek...)
	lock, err := finder.Lock()
	// All look-ahead bytes go back to the stream whether or not the lock
	// succeeded; on failure the regular frame read produces the error.
	d.source.Unread(finder.Bytes())
	if err != nil {
		return err
	}
	d.freeLock = lock
	return nil
}

// frameSizeOf returns the compressed size in bytes of the next frame, using
// the locked frame length for free-bitrate streams and the header's bitrate
// table entry for all other streams.
func (d *Decoder) frameSizeOf(h frameheader.FrameHeader) (int, error) {
	if d.freeLock != nil && h.IsFreeBitrate() {
		return d.freeLock.FrameSize(h), nil
	}
	return h.FrameSize()
}

// Read is io.Reader's Read.
func (d *Decoder) Read(buf []byte) (int, error) {
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
// Note that seek uses a byte offset but samples are aligned to 4 bytes (2
// channels, 2 bytes each). Be careful to seek to an offset that is divisible by
// 4 if you want to read at full sample boundaries.
func (d *Decoder) Seek(offset int64, whence int) (int64, error) {
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
		npos = d.Length() + offset
	default:
		return 0, errors.New("mp3: invalid whence")
	}
	d.pos = npos
	d.buf = nil
	d.frame = nil
	f := d.pos / d.bytesPerFrame
	// If the frame is not first, read the previous ahead of reading that
	// because the previous frame can affect the targeted frame.
	if f > 0 {
		f--
		if _, err := d.source.Seek(d.frameStarts[f], 0); err != nil {
			return 0, err
		}
		if err := d.readFrame(); err != nil {
			return 0, err
		}
		if err := d.readFrame(); err != nil {
			return 0, err
		}
		d.buf = d.buf[d.bytesPerFrame+(d.pos%d.bytesPerFrame):]
	} else {
		if _, err := d.source.Seek(d.frameStarts[f], 0); err != nil {
			return 0, err
		}
		if err := d.readFrame(); err != nil {
			return 0, err
		}
		d.buf = d.buf[d.pos:]
	}
	return npos, nil
}

// SampleRate returns the sample rate like 44100.
//
// Note that the sample rate is retrieved from the first frame.
func (d *Decoder) SampleRate() int {
	return d.sampleRate
}

func (d *Decoder) ensureFrameStartsAndLength() error {
	if d.length != invalidLength {
		return nil
	}

	if _, ok := d.source.reader.(io.Seeker); !ok {
		return nil
	}

	// Keep the current logical position and any queued look-ahead bytes.
	// A plain Seek(Current) would report the underlying reader's position,
	// which can run ahead of the decoder while look-ahead data is buffered.
	curPos := d.source.pos
	curBuf := append([]byte(nil), d.source.buf...)
	if err := d.source.rewind(); err != nil {
		return err
	}

	if err := d.source.skipTags(); err != nil {
		return err
	}
	l := int64(0)
	for {
		var h frameheader.FrameHeader
		pos := d.source.pos
		if d.freeLock != nil {
			// Locked framing: the next four bytes must be the header.
			fh, err := frameheader.ReadFixed(d.source)
			if err != nil {
				if err == io.EOF {
					break
				}
				if _, ok := err.(*consts.UnexpectedEOF); ok {
					break
				}
				return err
			}
			h = fh
		} else {
			fh, p, err := frameheader.Read(d.source, d.source.pos)
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
			h, pos = fh, p
		}
		framesize, err := d.frameSizeOf(h)
		if err != nil {
			return err
		}
		// A truncated final frame is not a complete audio frame: stop
		// without recording it, so Length counts complete frames only.
		if framesize < 4 {
			break
		}
		buf := make([]byte, framesize-4)
		if _, err := d.source.ReadFull(buf); err != nil {
			if err == io.EOF {
				break
			}
			if _, ok := err.(*consts.UnexpectedEOF); ok {
				break
			}
			return err
		}
		d.frameStarts = append(d.frameStarts, pos)
		d.bytesPerFrame = int64(h.BytesPerFrame())
		l += d.bytesPerFrame
	}
	d.length = l

	// Restore the logical position captured before the scan (right after
	// the first frame was read). The underlying reader has to be positioned
	// past the queued look-ahead bytes, which are replayed from buf first.
	if _, err := d.source.Seek(curPos+int64(len(curBuf)), io.SeekStart); err != nil {
		return err
	}
	d.source.buf = curBuf
	d.source.pos = curPos
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
	// Free-bitrate streams need their frame length locked from three
	// consecutive frame headers before the first frame can be read.
	if err := d.lockFreeFormat(); err != nil {
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

	if err := d.ensureFrameStartsAndLength(); err != nil {
		return nil, err
	}

	return d, nil
}
