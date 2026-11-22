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

// Package framelock determines the frame length of free-bitrate (bitrate
// index 0) MPEG-1/2 Layer III streams.
//
// A free-format header does not encode the frame length, and a sync word can
// appear inside main data, so the length cannot be found by scanning for the
// next sync word. Instead the stream is buffered (at most MaxBufferedBytes)
// and scanned for frame headers; a lock is accepted only when three
// consecutive, mutually compatible headers are found, i.e. the distance
// between the first and the second equals the distance between the second
// and the third after accounting for each frame's padding bit. Every byte
// consumed while searching is retained; the caller returns them to the
// stream in their original order, so look-ahead data is neither duplicated
// nor lost.
package framelock

import (
	"errors"
	"io"

	"github.com/hajimehoshi/go-mp3/internal/consts"
	"github.com/hajimehoshi/go-mp3/internal/frameheader"
)

// Limits mandated by the free-format support contract.
const (
	MinUnpaddedSize = 64
	MaxUnpaddedSize = 2000
	// MaxBufferedBytes bounds the compressed data held for one locking
	// attempt. Three frames span at most 3*MaxUnpaddedSize bytes, so a
	// conforming stream always locks within one window.
	MaxBufferedBytes = 8192
)

// ErrNotLocked is returned when the stream ends (or the buffering limit is
// reached) before three compatible frame headers could confirm the frame
// length.
var ErrNotLocked = errors.New("mp3: could not lock free bitrate frame length: need three consecutive frame headers")

// candidate keeps one observed frame header and the stream offset of the
// first byte of its sync word.
type candidate struct {
	offset  int64
	header  frameheader.FrameHeader
	padding int
}

// compatible reports whether two free-format headers describe the same fixed
// stream configuration. The MPEG version, layer, sample rate and channel mode
// must match (and both must use the free bitrate index). Three bits are
// allowed to differ per frame because they never change the frame length or
// the fixed format: the padding bit, the protection (CRC) bit and the
// application-defined private bit.
func compatible(a, b frameheader.FrameHeader) bool {
	const variableBits = 0x00010300 // protection 0x10000, padding 0x200, private 0x100
	const keepMask = ^frameheader.FrameHeader(variableBits)
	return a.BitrateIndex() == 0 && b.BitrateIndex() == 0 &&
		a&keepMask == b&keepMask
}

// Lock holds the locked frame length of a free-format stream.
type Lock struct {
	// header is the reference frame header; the padding bit of later
	// frames may differ, all other header fields stay fixed.
	header frameheader.FrameHeader
	// unpaddedSize is the constant frame length without the padding byte.
	unpaddedSize int
}

// Header returns the reference frame header. Its padding bit reflects the
// header of the first locked frame and must not be assumed for later frames.
func (l *Lock) Header() frameheader.FrameHeader {
	return l.header
}

// UnpaddedSize returns the constant frame length in bytes without padding.
func (l *Lock) UnpaddedSize() int {
	return l.unpaddedSize
}

// FrameSize returns the total size in bytes of the frame described by h,
// including its padding byte.
func (l *Lock) FrameSize(h frameheader.FrameHeader) int {
	return l.unpaddedSize + int(h.PaddingBit())
}

// FullReader is the byte source the finder scans, matching the readers used
// by the other internal packages.
type FullReader interface {
	ReadFull([]byte) (int, error)
}

// Finder locates the first three compatible frame headers of a free-format
// stream and locks the frame length.
type Finder struct {
	source   FullReader
	startPos int64
	// buf holds every byte consumed from the source, in order, and never
	// exceeds MaxBufferedBytes.
	buf []byte
	// candidates are frame headers found inside buf, sorted by offset.
	candidates []candidate
	eof        bool
}

// NewFinder starts a frame-length search at the given stream position.
// initial, if any, are bytes already consumed from the source at that
// position; they are scanned together with the bytes read afterwards and
// are included in Bytes.
func NewFinder(source FullReader, position int64, initial ...byte) *Finder {
	f := &Finder{
		source:   source,
		startPos: position,
	}
	f.buf = append(f.buf, initial...)
	return f
}

// Bytes returns every byte consumed so far, in stream order. The caller is
// responsible for returning them to the stream.
func (f *Finder) Bytes() []byte {
	return f.buf
}

// AtEOF reports whether the underlying source reached EOF.
func (f *Finder) AtEOF() bool {
	return f.eof
}

// Lock scans the source until three compatible headers confirm the frame
// length, or the source ends (or the buffering limit is reached) without a
// confirmation. Reading one byte at a time keeps sources that hand data over
// a single byte per Read working identically.
func (f *Finder) Lock() (*Lock, error) {
	// Inspect any seeded bytes first.
	for i := 0; i < len(f.buf); i++ {
		f.consider(i)
	}
	one := make([]byte, 1)
	for len(f.buf) < MaxBufferedBytes {
		n, err := f.source.ReadFull(one)
		if n > 0 {
			f.buf = append(f.buf, one[0])
			f.consider(len(f.buf) - 1)
		}
		if err != nil {
			if err == io.EOF {
				f.eof = true
				break
			}
			return nil, err
		}
	}
	return f.result()
}

// consider checks whether a frame header ends at index i; if so it is
// recorded as a candidate.
func (f *Finder) consider(i int) {
	if i < 3 {
		return
	}
	h := frameheader.FrameHeader(uint32(f.buf[i-3])<<24 |
		uint32(f.buf[i-2])<<16 |
		uint32(f.buf[i-1])<<8 |
		uint32(f.buf[i]))
	if !h.IsValid() || h.BitrateIndex() != 0 {
		return
	}
	if h.ID() == consts.Version2_5 || h.Layer() != consts.Layer3 {
		return
	}
	f.candidates = append(f.candidates, candidate{
		offset:  f.startPos + int64(i-3),
		header:  h,
		padding: int(h.PaddingBit()),
	})
}

// result evaluates the recorded candidates. For every compatible pair it
// computes the implied unpadded length and checks whether a third
// compatible header sits exactly at the predicted next-frame offset. An
// offset index keeps this O(n^2) so a stream packed with decoy headers
// cannot turn the search into a cubic scan.
func (f *Finder) result() (*Lock, error) {
	byOffset := make(map[int64]candidate, len(f.candidates))
	for _, c := range f.candidates {
		byOffset[c.offset] = c
	}
	for i := 0; i+1 < len(f.candidates); i++ {
		c0 := f.candidates[i]
		for j := i + 1; j < len(f.candidates); j++ {
			c1 := f.candidates[j]
			if !compatible(c0.header, c1.header) {
				continue
			}
			unpadded := c1.offset - c0.offset - int64(c0.padding)
			if unpadded < MinUnpaddedSize || unpadded > MaxUnpaddedSize {
				continue
			}
			c2, ok := byOffset[c1.offset+unpadded+int64(c1.padding)]
			if !ok || !compatible(c1.header, c2.header) {
				continue
			}
			return &Lock{
				header:       c0.header,
				unpaddedSize: int(unpadded),
			}, nil
		}
	}
	return nil, ErrNotLocked
}
