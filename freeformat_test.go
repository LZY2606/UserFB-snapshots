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
	"bytes"
	"encoding/binary"
	"io"
	"os"
	"strings"
	"testing"

	"github.com/hajimehoshi/go-mp3/internal/frameheader"
	"github.com/hajimehoshi/go-mp3/internal/framelock"
	"github.com/hajimehoshi/go-mp3/internal/sideinfo"
	"github.com/hajimehoshi/go-mp3/task_support/taskcheck"
)

// meta describes one frame parsed straight from a fixture file. Everything
// here is computed with Go code; no external codec is involved.
type meta struct {
	raw       []byte
	header    frameheader.FrameHeader
	size      int // total frame size in the fixture
	siSize    int
	mainSize  int
	mdb       int // side_info main_data_begin (reservoir offset in bytes)
	cbits     int // sum of part2_3 lengths: Huffman/scalefactor bits consumed
	pad       int
	protected bool // protection bit set, i.e. no CRC word
}

type sideInfoReader struct {
	data []byte
	off  int
}

func (r *sideInfoReader) ReadFull(p []byte) (int, error) {
	n := copy(p, r.data[r.off:])
	r.off += n
	if n < len(p) {
		return n, io.EOF
	}
	return n, nil
}

// parseFixtureFrames walks a fixture file exactly like the decoder does and
// records per-frame metadata used to construct free-format variants.
func parseFixtureFrames(t *testing.T, filename string) []meta {
	t.Helper()
	raw, err := os.ReadFile("example/" + filename)
	if err != nil {
		t.Fatal(err)
	}
	var out []meta
	for p := 0; p+4 <= len(raw); {
		h := frameheader.FrameHeader(binary.BigEndian.Uint32(raw[p : p+4]))
		if !h.IsValid() || h.Layer() != 1 || h.BitrateIndex() == 0 {
			p++
			continue
		}
		size, e := h.FrameSize()
		if e != nil || size < 4 || p+size > len(raw) {
			p++
			continue
		}
		off := 4
		if h.ProtectionBit() == 0 {
			off += 2
		}
		si, err := sideinfo.Read(&sideInfoReader{data: raw[p+off:]}, h, size)
		if err != nil {
			break
		}
		c := 0
		for gr := 0; gr < h.Granules(); gr++ {
			for ch := 0; ch < h.NumberOfChannels(); ch++ {
				c += si.Part2_3Length[gr][ch]
			}
		}
		mainSize := size - 4 - h.SideInfoSize() - (1-h.ProtectionBit())*2
		out = append(out, meta{
			raw:       append([]byte(nil), raw[p:p+size]...),
			header:    h,
			size:      size,
			siSize:    h.SideInfoSize(),
			mainSize:  mainSize,
			mdb:       si.MainDataBegin,
			cbits:     c,
			pad:       h.PaddingBit(),
			protected: h.ProtectionBit() == 1,
		})
		p += size
	}
	return out
}

// unpaddedRun returns n consecutive frames whose unpadded length
// (frame size minus the padding byte) is constant. Padding may differ per
// frame; this matches the fixture selection rule size-padding and is what
// makes per-frame padding tests possible.
func unpaddedRun(frames []meta, n int) []meta {
	for start := 0; start+n <= len(frames); start++ {
		base := frames[start].size - frames[start].pad
		ok := true
		for k := 0; k < n; k++ {
			f := frames[start+k]
			if f.size-f.pad != base {
				ok = false
				break
			}
		}
		if ok {
			return append([]meta(nil), frames[start:start+n]...)
		}
	}
	return nil
}

// withBitrateIndexZero returns a copy of the frame whose header bitrate
// index is cleared to 0 (free format). Only the high nibble of header byte 2
// changes; every other byte, including all main-data bytes, is untouched.
func withBitrateIndexZero(raw []byte) []byte {
	out := append([]byte(nil), raw...)
	out[2] &= 0x0f
	return out
}

// setPadding rewrites the padding bit and adds or removes the single Layer
// III padding slot. The padding slot sits after main data and is never read
// as scalefactor or Huffman data.
func setPadding(m meta, wantPad int) []byte {
	h := m.header
	body := append([]byte(nil), m.raw...)
	body = body[:len(body)-m.pad] // normalize: drop any existing padding slot
	if wantPad == 1 {
		h = frameheader.FrameHeader(uint32(m.header) | 0x200)
		body = append(body, 0)
	} else {
		h = frameheader.FrameHeader(uint32(m.header) &^ 0x200)
	}
	var hb [4]byte
	binary.BigEndian.PutUint32(hb[:], uint32(h))
	copy(body[:4], hb[:])
	return body
}

// addCRC converts a no-CRC frame into a CRC-protected frame without changing
// its total size: a two-byte CRC word is inserted after the header and two
// provably dead main-data bytes (main-data offsets [0,2), shown by the
// caller to carry no Huffman bits and to lie outside the next frame's
// reservoir reach) are removed from the end of that dead region.
func addCRC(m meta) []byte {
	h := frameheader.FrameHeader(uint32(m.header) &^ 0x10000)
	var hb [4]byte
	binary.BigEndian.PutUint32(hb[:], uint32(h))
	main := m.raw[4+m.siSize:]
	out := make([]byte, 0, len(m.raw))
	out = append(out, hb[:]...)
	out = append(out, 0, 0) // CRC word; the decoder skips it
	out = append(out, m.raw[4:4+m.siSize]...)
	out = append(out, main[2:]...) // drop the two dead prefix bytes
	return out
}

// placeFakeHeader writes a syntactically valid free-format header word into
// main data. The word matches the surrounding stream's version, sample rate
// and channel mode (only the bitrate index is cleared), so it is a maximal
// distractor: the framer must reject it purely on spacing, not on a header
// field mismatch. The caller proves that the target four bytes never
// participate in Huffman decoding (see the cbits / reservoir-reach checks).
func placeFakeHeader(raw []byte, siSize, mainOffset int, template frameheader.FrameHeader) []byte {
	out := append([]byte(nil), raw...)
	pos := 4 + siSize + mainOffset
	fake := frameheader.FrameHeader(uint32(template) &^ 0x0000f000) // index 0
	var hb [4]byte
	binary.BigEndian.PutUint32(hb[:], uint32(fake))
	copy(out[pos:pos+4], hb[:])
	return out
}

// buildStreams concatenates mutated frames and returns both the regular
// (bitrate-table) reference stream and its free-format twin.
func buildStreams(mutated [][]byte) (orig, free []byte) {
	for _, b := range mutated {
		orig = append(orig, b...)
		free = append(free, withBitrateIndexZero(b)...)
	}
	return orig, free
}

func decodeAll(t *testing.T, r io.Reader) ([]byte, *Decoder) {
	t.Helper()
	d, err := NewDecoder(r)
	if err != nil {
		t.Fatal(err)
	}
	pcm, err := io.ReadAll(d)
	if err != nil {
		t.Fatal(err)
	}
	return pcm, d
}

// oneByteReader hands over exactly one byte per Read, the worst case for any
// look-ahead buffering.
type oneByteReader struct {
	data []byte
	off  int
}

func (r *oneByteReader) Read(p []byte) (int, error) {
	if r.off >= len(r.data) {
		return 0, io.EOF
	}
	p[0] = r.data[r.off]
	r.off++
	return 1, nil
}

// chunkReader hands over at most maxChunk bytes per Read.
type chunkReader struct {
	data     []byte
	off      int
	maxChunk int
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if r.off >= len(r.data) {
		return 0, io.EOF
	}
	n := len(r.data) - r.off
	if n > r.maxChunk {
		n = r.maxChunk
	}
	if n > len(p) {
		n = len(p)
	}
	copy(p[:n], r.data[r.off:r.off+n])
	r.off += n
	return n, nil
}

// countingReader records the largest Read buffer it was asked to fill,
// proving the decoder never requests the whole file at once.
type countingReader struct {
	data     []byte
	off      int
	maxAsked int
}

func (r *countingReader) Read(p []byte) (int, error) {
	if len(p) > r.maxAsked {
		r.maxAsked = len(p)
	}
	if r.off >= len(r.data) {
		return 0, io.EOF
	}
	n := copy(p, r.data[r.off:])
	r.off += n
	return n, nil
}

func isNotZero(b []byte) bool {
	for _, x := range b {
		if x != 0 {
			return true
		}
	}
	return false
}

// pickDeadPrefixWindow finds a silent frame (no Huffman bits) whose first
// deadPrefix main-data bytes are outside the next frame's reservoir. It
// returns the frame index and the start of an n-frame window containing it
// that also includes at least one non-silent frame, so the resulting stream
// carries real audio. (-1, 0) means no suitable frame was found.
func pickDeadPrefixWindow(frames []meta, deadPrefix, n int) (pick, start int) {
	for i := 0; i+1 < len(frames); i++ {
		f := frames[i]
		if !(f.cbits == 0 && f.protected && f.mainSize-frames[i+1].mdb >= deadPrefix) {
			continue
		}
		lo := i - (n - 1)
		if lo < 0 {
			lo = 0
		}
		hi := i
		if hi+n > len(frames) {
			hi = len(frames) - n
		}
		for s := lo; s <= hi; s++ {
			if s < 0 || s+n > len(frames) {
				continue
			}
			hasAudio := false
			for k := 0; k < n; k++ {
				if frames[s+k].cbits > 0 {
					hasAudio = true
					break
				}
			}
			if hasAudio {
				return i, s
			}
		}
	}
	return -1, 0
}

// TestFreeFormatPCM verifies that free-format streams decode to exactly the
// same 16-bit little-endian stereo PCM as their bitrate-table originals,
// across padding, CRC and fake-header-in-payload constructions.
func TestFreeFormatPCM(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		classic := parseFixtureFrames(t, "classic.mp3")
		mpeg2 := parseFixtureFrames(t, "mpeg2.mp3")

		// 1. Plain free-format conversion of continuous, unpadded,
		// equal-length frames from both fixtures.
		for _, tc := range []struct {
			name   string
			frames []meta
		}{
			{"classic", unpaddedRun(classic, 8)},
			{"mpeg2", unpaddedRun(mpeg2, 8)},
		} {
			mutated := make([][]byte, len(tc.frames))
			for i, f := range tc.frames {
				mutated[i] = append([]byte(nil), f.raw...)
			}
			orig, free := buildStreams(mutated)
			ref, rd := decodeAll(t, bytes.NewReader(orig))
			got, fd := decodeAll(t, bytes.NewReader(free))
			c.Check(isNotZero(ref), "%s: reference PCM must not be silent", tc.name)
			c.Check(bytes.Equal(got, ref), "%s: free-format PCM must match the original byte for byte", tc.name)
			c.Check(len(got)%4 == 0, "%s: output stays 16bit stereo (4 bytes/sample)", tc.name)
			c.Check(fd.SampleRate() == rd.SampleRate(), "%s: sample rate preserved (%d)", tc.name, rd.SampleRate())
			c.Check(fd.Length() == int64(len(ref)), "%s: length counts every frame's PCM", tc.name)
		}

		// 2. Per-frame varying padding: padding may differ on every frame
		// while the unpadded length stays constant.
		for _, tc := range []struct {
			name   string
			frames []meta
		}{
			{"classic", unpaddedRun(classic, 8)},
			{"mpeg2", unpaddedRun(mpeg2, 8)},
		} {
			pattern := []int{0, 1, 1, 0, 1, 0, 0, 1}
			mutated := make([][]byte, len(tc.frames))
			for i, f := range tc.frames {
				mutated[i] = setPadding(f, pattern[i])
			}
			orig, free := buildStreams(mutated)
			ref, _ := decodeAll(t, bytes.NewReader(orig))
			got, _ := decodeAll(t, bytes.NewReader(free))
			c.Check(isNotZero(ref), "%s var-pad: reference PCM must not be silent", tc.name)
			c.Check(bytes.Equal(got, ref), "%s var-pad: free-format PCM must match with changing padding", tc.name)
		}

		// 3. CRC-protected free-format frames. A silent (cbits==0) frame is
		// used so the two removed bytes can be proven unused, while the rest
		// of the run carries non-silent audio.
		crcPick, crcStart := pickDeadPrefixWindow(mpeg2, 2, 8)
		c.Check(crcPick >= 0, "mpeg2: need a silent frame with a provably dead main-data prefix")
		if crcPick >= 0 {
			mutated := make([][]byte, 8)
			for k := 0; k < 8; k++ {
				f := mpeg2[crcStart+k]
				if k == crcPick-crcStart {
					// Proof for the removed prefix bytes: this frame has no
					// Huffman/scalefactor bits of its own, and its first
					// (mainSize - next.mdb) main-data bytes are outside the
					// next frame's reservoir, so no decoder ever reads them.
					c.Check(f.cbits == 0, "crc: chosen frame has no Huffman bits")
					c.Check(f.mainSize-mpeg2[crcStart+k+1].mdb >= 2,
						"crc: removed bytes lie outside the next frame's reservoir")
					mutated[k] = addCRC(f)
				} else {
					mutated[k] = append([]byte(nil), f.raw...)
				}
			}
			orig, free := buildStreams(mutated)
			ref, _ := decodeAll(t, bytes.NewReader(orig))
			got, _ := decodeAll(t, bytes.NewReader(free))
			c.Check(isNotZero(ref), "crc: reference stream must contain non-silent frames")
			c.Check(bytes.Equal(got, ref), "crc: free-format PCM must match with CRC words present")
		}

		// 4. A valid header word planted inside main data must not disturb
		// decoding or framing. Two placements are exercised: an internal
		// frame's dead prefix (before the third real header is even locked)
		// and the last frame's never-read tail.
		fakePick, fakeStart := pickDeadPrefixWindow(mpeg2, 4, 8)
		c.Check(fakePick >= 0, "fake: need an internal frame with a dead prefix")
		if fakePick >= 0 {
			mutated := make([][]byte, 8)
			for k := 0; k < 8; k++ {
				f := mpeg2[fakeStart+k]
				if k == fakePick-fakeStart {
					c.Check(f.cbits == 0, "fake: planted bytes are not Huffman-coded bits")
					c.Check(f.mainSize-mpeg2[fakeStart+k+1].mdb >= 4,
						"fake: planted bytes are outside the next frame's reservoir")
					mutated[k] = placeFakeHeader(f.raw, f.siSize, 0, f.header)
					// The planted word must itself be a valid free header.
					planted := frameheader.FrameHeader(binary.BigEndian.Uint32(
						mutated[k][4+f.siSize:]))
					c.Check(planted.IsValid() && planted.IsFreeBitrate(),
						"fake: planted word must be a valid free-format header")
				} else {
					mutated[k] = append([]byte(nil), f.raw...)
				}
			}
			orig, free := buildStreams(mutated)
			ref, _ := decodeAll(t, bytes.NewReader(orig))
			got, fd := decodeAll(t, bytes.NewReader(free))
			c.Check(isNotZero(ref), "fake: reference stream must contain non-silent audio")
			c.Check(bytes.Equal(got, ref), "fake: payload header must not change the PCM")
			c.Check(len(fd.frameStarts) == 8, "fake: payload header must not create an extra frame boundary")
			c.Check(fd.freeLock.UnpaddedSize() == mpeg2[fakeStart].size-mpeg2[fakeStart].pad,
				"fake: locked frame length must be the true one")
		}

		// Last-frame tail placement: bytes past the final frame's own
		// Huffman consumption can never be read by anyone.
		last := mpeg2[len(mpeg2)-1]
		tailStart := (last.cbits + 7) / 8
		c.Check(last.cbits > 0, "tail fake: last frame carries real audio")
		c.Check(last.mainSize-tailStart >= 4, "tail fake: unread tail has room for a fake header")
		if last.mainSize-tailStart >= 4 {
			const n = 8
			mutated := make([][]byte, n)
			for k := 0; k < n; k++ {
				f := mpeg2[len(mpeg2)-n+k]
				if k == n-1 {
					mutated[k] = placeFakeHeader(f.raw, f.siSize, tailStart, f.header)
					planted := frameheader.FrameHeader(binary.BigEndian.Uint32(
						mutated[k][4+f.siSize+tailStart:]))
					c.Check(planted.IsValid() && planted.IsFreeBitrate(),
						"tail fake: planted word must be a valid free-format header")
				} else {
					mutated[k] = append([]byte(nil), f.raw...)
				}
			}
			orig, free := buildStreams(mutated)
			ref, _ := decodeAll(t, bytes.NewReader(orig))
			got, _ := decodeAll(t, bytes.NewReader(free))
			c.Check(isNotZero(ref), "tail fake: reference PCM must not be silent")
			c.Check(bytes.Equal(got, ref), "tail fake: PCM must match with a header in the final tail")
		}
	})
}

// TestFreeFormatFraming verifies frame-length locking, truncation handling
// and the 8192-byte look-ahead bound.
func TestFreeFormatFraming(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		c.Check(framelock.MaxBufferedBytes == 8192, "look-ahead buffer cap must be 8192 bytes")

		for _, name := range []string{"classic", "mpeg2"} {
			frames := unpaddedRun(parseFixtureFrames(t, name+".mp3"), 8)
			c.Check(frames != nil, "%s: fixture must offer an 8-frame unpadded run", name)
			if frames == nil {
				continue
			}
			mutated := make([][]byte, len(frames))
			for i, f := range frames {
				mutated[i] = append([]byte(nil), f.raw...)
			}
			orig, free := buildStreams(mutated)

			// Locked unpadded length is in range and matches the fixture.
			_, fd := decodeAll(t, bytes.NewReader(free))
			wantUnpadded := frames[0].size - frames[0].pad
			c.Check(fd.freeLock.UnpaddedSize() >= framelock.MinUnpaddedSize &&
				fd.freeLock.UnpaddedSize() <= framelock.MaxUnpaddedSize,
				"%s: locked unpadded length %d in [64,2000]", name, fd.freeLock.UnpaddedSize())
			c.Check(fd.freeLock.UnpaddedSize() == wantUnpadded,
				"%s: locked length %d must equal the fixture frame length %d",
				name, fd.freeLock.UnpaddedSize(), wantUnpadded)

			// frameStarts must equal the true cumulative compressed offsets.
			expected := make([]int64, len(frames))
			var off int64
			for i := range frames {
				expected[i] = off
				off += int64(fd.freeLock.FrameSize(
					frameheader.FrameHeader(binary.BigEndian.Uint32(mutated[i][:4]))))
			}
			c.Check(len(fd.frameStarts) == len(frames), "%s: one start per frame", name)
			c.Check(int64SliceEqual(fd.frameStarts, expected),
				"%s: frame starts must follow the locked spacing", name)

			// Reference stream still decodes through the bitrate table path.
			ref, rd := decodeAll(t, bytes.NewReader(orig))
			c.Check(rd.freeLock == nil, "%s: table-bitrate streams must not take the free-format path", name)
			c.Check(isNotZero(ref), "%s: reference PCM must not be silent", name)

			// Truncated final frame: every cut from 1 byte up to a full frame
			// yields exactly the complete frames' PCM and no partial frame.
			fullPCM, _ := decodeAll(t, bytes.NewReader(free))
			framePCM := int(fd.bytesPerFrame)
			for _, cut := range []int{1, 3, 7, wantUnpadded - 1} {
				if cut >= len(free) {
					continue
				}
				trunc := free[:len(free)-cut]
				td, err := NewDecoder(bytes.NewReader(trunc))
				c.Check(err == nil, "%s cut %d: locking must still succeed on the complete prefix", name, cut)
				if err != nil {
					continue
				}
				pcm, rerr := io.ReadAll(td)
				want := fullPCM[:framePCM*(len(frames)-1)]
				c.Check(bytes.Equal(pcm, want), "%s cut %d: truncated tail must not produce PCM", name, cut)
				c.Check(rerr == nil, "%s cut %d: complete frames must decode without a read error", name, cut)
				c.Check(td.Length() == int64(len(want)), "%s cut %d: Length counts complete frames only", name, cut)
			}

			// Streams that cannot supply three headers must fail loudly.
			oneFrame := withBitrateIndexZero(mutated[0])
			twoFrames := append(withBitrateIndexZero(mutated[0]),
				withBitrateIndexZero(mutated[1])...)
			for _, short := range [][]byte{oneFrame, twoFrames} {
				_, err := NewDecoder(bytes.NewReader(short))
				c.Check(err != nil && strings.Contains(err.Error(), "lock"),
					"%s: fewer than three headers must return a locking error", name)
			}
		}
	})
}

func int64SliceEqual(a, b []int64) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// TestFreeFormatSources verifies behavior across seekable and streaming
// sources, tiny reads, length reporting and independent decoder instances.
func TestFreeFormatSources(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		classic := unpaddedRun(parseFixtureFrames(t, "classic.mp3"), 8)
		mpeg2 := unpaddedRun(parseFixtureFrames(t, "mpeg2.mp3"), 8)
		c.Check(classic != nil && mpeg2 != nil, "both fixtures must provide unpadded runs")

		mkFree := func(frames []meta) []byte {
			mutated := make([][]byte, len(frames))
			for i, f := range frames {
				mutated[i] = withBitrateIndexZero(f.raw)
			}
			_, free := buildStreams(mutated)
			return free
		}
		classicFree := mkFree(classic)
		mpeg2Free := mkFree(mpeg2)
		classicRef, _ := decodeAll(t, bytes.NewReader(classicFree))
		mpeg2Ref, _ := decodeAll(t, bytes.NewReader(mpeg2Free))

		// 1. Identical PCM from a seeker, a one-byte reader and tiny chunks.
		for _, tc := range []struct {
			name string
			free []byte
			ref  []byte
		}{
			{"classic", classicFree, classicRef},
			{"mpeg2", mpeg2Free, mpeg2Ref},
		} {
			fromSeeker, _ := decodeAll(t, bytes.NewReader(tc.free))
			fromOne, _ := decodeAll(t, &oneByteReader{data: tc.free})
			fromChunk, _ := decodeAll(t, &chunkReader{data: tc.free, maxChunk: 3})
			c.Check(bytes.Equal(fromSeeker, tc.ref), "%s: seeker PCM matches", tc.name)
			c.Check(bytes.Equal(fromOne, tc.ref), "%s: one-byte Reader yields identical PCM", tc.name)
			c.Check(bytes.Equal(fromChunk, tc.ref), "%s: 3-byte-chunk Reader yields identical PCM", tc.name)
		}

		// 2. Length is -1 for non-seekable sources and exact for seekable ones.
		nonSeek, err := NewDecoder(&oneByteReader{data: classicFree})
		c.Check(err == nil, "non-seekable decoder construction must succeed")
		c.Check(nonSeek.Length() == -1, "non-seekable Length must be -1")
		nonSeekPCM, _ := io.ReadAll(nonSeek)
		c.Check(bytes.Equal(nonSeekPCM, classicRef), "non-seekable PCM matches the reference")

		seekDec, err := NewDecoder(bytes.NewReader(mpeg2Free))
		c.Check(err == nil, "seekable decoder construction must succeed")
		c.Check(seekDec.Length() == int64(len(mpeg2Ref)), "seekable Length equals the full PCM size")

		// 3. Seek back to 0 re-reads the whole stream identically.
		n, serr := seekDec.Seek(0, io.SeekStart)
		c.Check(serr == nil && n == 0, "Seek(0, Start) must succeed")
		reread, _ := io.ReadAll(seekDec)
		c.Check(bytes.Equal(reread, mpeg2Ref), "re-reading after Seek(0) must reproduce all PCM")

		// 4. Two decoder instances keep independent state while interleaved.
		dA, err := NewDecoder(bytes.NewReader(classicFree))
		c.Check(err == nil, "decoder A construction must succeed")
		dB, err := NewDecoder(bytes.NewReader(mpeg2Free))
		c.Check(err == nil, "decoder B construction must succeed")
		var pcmA, pcmB []byte
		tail := make([]byte, 5)
		for {
			nA, errA := dA.Read(tail)
			pcmA = append(pcmA, tail[:nA]...)
			nB, errB := dB.Read(tail)
			pcmB = append(pcmB, tail[:nB]...)
			if errA == io.EOF && errB == io.EOF {
				break
			}
			c.Check(errA == nil || errA == io.EOF, "interleaved read A must not fail")
			c.Check(errB == nil || errB == io.EOF, "interleaved read B must not fail")
		}
		c.Check(bytes.Equal(pcmA, classicRef), "interleaved decoder A produces its own PCM")
		c.Check(bytes.Equal(pcmB, mpeg2Ref), "interleaved decoder B produces its own PCM")

		// 5. The decoder never asks for the whole file in one Read; the
		// largest request is bounded by a frame-sized buffer, not the stream.
		cr := &countingReader{data: classicFree}
		crDec, err := NewDecoder(cr)
		c.Check(err == nil, "counting-reader decoder construction must succeed")
		_, _ = io.ReadAll(crDec)
		c.Check(cr.maxAsked > 0 && cr.maxAsked < len(classicFree),
			"no single Read may request the whole file (maxAsked=%d, file=%d)",
			cr.maxAsked, len(classicFree))
	})
}
