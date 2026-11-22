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

package framelock

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"

	"github.com/hajimehoshi/go-mp3/internal/frameheader"
)

// byteReader adapts a byte slice to the FullReader used by Finder.
type byteReader struct{ data []byte }

func (r *byteReader) ReadFull(p []byte) (int, error) {
	n := copy(p, r.data)
	r.data = r.data[n:]
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

// makeHeader builds a free-format MPEG-1 Layer III 44.1kHz stereo header with
// the given padding bit.
func makeHeader(pad int) frameheader.FrameHeader {
	word := uint32(0xfffb0004) // sync, MPEG-1, Layer III, free bitrate, 44.1k, stereo
	if pad == 1 {
		word |= 0x200
	}
	return frameheader.FrameHeader(word)
}

func writeHeader(buf []byte, h frameheader.FrameHeader) {
	binary.BigEndian.PutUint32(buf, uint32(h))
}

// buildStream lays n frames of constant unpadded size with per-frame padding.
func buildStream(n, unpadded int, pads []int) []byte {
	var out []byte
	for i := 0; i < n; i++ {
		frame := make([]byte, unpadded+pads[i])
		writeHeader(frame, makeHeader(pads[i]))
		out = append(out, frame...)
	}
	return out
}

func lockFrom(t *testing.T, data []byte) *Lock {
	t.Helper()
	f := NewFinder(&byteReader{data: data}, 0)
	l, err := f.Lock()
	if err != nil {
		t.Fatalf("Lock failed: %v", err)
	}
	return l
}

func TestFinderLocksBasic(t *testing.T) {
	stream := buildStream(4, 100, []int{0, 1, 0, 1})
	l := lockFrom(t, stream)
	if l.UnpaddedSize() != 100 {
		t.Fatalf("unpadded size: got %d, want 100", l.UnpaddedSize())
	}
	if l.FrameSize(makeHeader(1)) != 101 || l.FrameSize(makeHeader(0)) != 100 {
		t.Fatal("FrameSize must add exactly the padding byte")
	}
}

func TestFinderSizeBounds(t *testing.T) {
	for _, size := range []int{63, 2001} {
		stream := buildStream(3, size, []int{0, 0, 0})
		f := NewFinder(&byteReader{data: stream}, 0)
		if _, err := f.Lock(); !errors.Is(err, ErrNotLocked) {
			t.Fatalf("size %d: got err %v, want ErrNotLocked", size, err)
		}
	}
	for _, size := range []int{64, 2000} {
		stream := buildStream(3, size, []int{0, 0, 0})
		l := lockFrom(t, stream)
		if l.UnpaddedSize() != size {
			t.Fatalf("size %d: locked %d", size, l.UnpaddedSize())
		}
	}
}

func TestFinderNeedsThreeHeaders(t *testing.T) {
	stream := buildStream(2, 100, []int{0, 0})
	f := NewFinder(&byteReader{data: stream}, 0)
	if _, err := f.Lock(); !errors.Is(err, ErrNotLocked) {
		t.Fatalf("two headers: got %v, want ErrNotLocked", err)
	}
	// An empty stream reaches EOF with no candidates.
	f = NewFinder(&byteReader{data: nil}, 0)
	if _, err := f.Lock(); !errors.Is(err, ErrNotLocked) {
		t.Fatalf("empty stream: got %v, want ErrNotLocked", err)
	}
}

func TestFinderIgnoresDistractor(t *testing.T) {
	// A valid free header planted inside the first frame must not be used:
	// it does not repeat at the true frame spacing, so the lock must still
	// resolve to the real frame length.
	stream := buildStream(4, 200, []int{0, 0, 0, 0})
	writeHeader(stream[40:44], makeHeader(0)) // distractor, not frame-aligned
	l := lockFrom(t, stream)
	if l.UnpaddedSize() != 200 {
		t.Fatalf("distractor changed lock: got %d, want 200", l.UnpaddedSize())
	}
}

func TestFinderRejectsIncompatibleHeader(t *testing.T) {
	// Two different sample-rate headers must never lock together even if
	// spaced evenly.
	base := buildStream(3, 100, []int{0, 0, 0})
	// Change the sampling-frequency field (bits 11-10) of the second header.
	base[100+2] |= 0x04
	f := NewFinder(&byteReader{data: base}, 0)
	if _, err := f.Lock(); !errors.Is(err, ErrNotLocked) {
		t.Fatalf("incompatible sample rate: got %v, want ErrNotLocked", err)
	}
}

func TestFinderReturnsAllBytes(t *testing.T) {
	stream := buildStream(4, 100, []int{0, 1, 0, 1})
	src := &byteReader{data: stream}
	f := NewFinder(src, 0)
	if _, err := f.Lock(); err != nil {
		t.Fatal(err)
	}
	// Every consumed byte must be available for replay via Bytes().
	got := f.Bytes()
	consumed := len(stream) - len(src.data)
	if !bytes.Equal(got, stream[:consumed]) {
		t.Fatalf("replay bytes mismatch (consumed=%d, buffered=%d)", consumed, len(got))
	}
}

func TestFinderBufferCap(t *testing.T) {
	if MaxBufferedBytes != 8192 {
		t.Fatalf("buffer cap: got %d, want 8192", MaxBufferedBytes)
	}
	// A long stream without a valid triple must not buffer past the cap.
	garbage := bytes.Repeat([]byte{0x00}, MaxBufferedBytes+100)
	garbage[0], garbage[1] = 0xff, 0xfb // one header, then nothing compatible
	f := NewFinder(&byteReader{data: garbage}, 0)
	_, _ = f.Lock()
	if len(f.Bytes()) > MaxBufferedBytes {
		t.Fatalf("buffered %d bytes, cap is %d", len(f.Bytes()), MaxBufferedBytes)
	}
}
