// Copyright 2015, Joe Tsai. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.md file.

package bzip2

import (
	"bytes"
	stdbzip "compress/bzip2"
	stderrors "errors"
	"io"
	"math/rand"
	"strings"
	"sync"
	"testing"

	cerr "github.com/dsnet/compress/internal/errors"
)

// compressMember compresses data with this package's Writer.
func compressMember(t *testing.T, data []byte, level int) []byte {
	t.Helper()
	var buf bytes.Buffer
	wr, err := NewWriter(&buf, &WriterConfig{Level: level})
	if err != nil {
		t.Fatalf("NewWriter(level=%d): %v", level, err)
	}
	if _, err := wr.Write(data); err != nil {
		t.Fatalf("Write: %v", err)
	}
	if err := wr.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	return buf.Bytes()
}

// archive holds one concatenated bzip2 archive and the expected plaintext.
type archive struct {
	compressed []byte
	plain      []byte
	levels     []int
}

// buildArchive concatenates several independently compressed members, some
// empty, some multi-block, at a mix of compression levels.
func buildArchive(t *testing.T) archive {
	t.Helper()
	parts := [][]byte{
		bytes.Repeat([]byte("record\x00key=value\n"), 20000), // ~260k, multi-block
		nil,                                 // Empty stream
		bytes.Repeat([]byte{0x5a}, 1200000), // Long RLE1 fill
		[]byte("short middle stream"),
		nil, // Another empty stream, sandwiched between non-empty ones
		pseudoRandom(305017),
	}
	levels := []int{1, 9, 1, 6, 2, 9}

	var compressed, plain bytes.Buffer
	for i, p := range parts {
		member := compressMember(t, p, levels[i])
		// Independently verify every member with the standard library.
		got, err := io.ReadAll(stdbzip.NewReader(bytes.NewReader(member)))
		if err != nil {
			t.Fatalf("standard library rejects member %d: %v", i, err)
		}
		if !bytes.Equal(got, p) {
			t.Fatalf("member %d plaintext mismatch", i)
		}
		compressed.Write(member)
		plain.Write(p)
	}
	return archive{compressed: compressed.Bytes(), plain: plain.Bytes(), levels: levels}
}

// pseudoRandom returns deterministic, incompressible-looking bytes.
func pseudoRandom(n int) []byte {
	r := rand.New(rand.NewSource(0x5a17))
	b := make([]byte, n)
	r.Read(b)
	return b
}

// flipBit flips one bit addressed from the MSB of the first byte.
func flipBit(d []byte, bit int64) {
	d[bit/8] ^= 1 << uint(7-bit%8)
}

// rangeRecorder records every byte range touched on an underlying ReaderAt.
type rangeRecorder struct {
	r     io.ReaderAt
	mu    sync.Mutex
	calls []readCall
}

type readCall struct {
	off int64
	end int64
}

func (rr *rangeRecorder) ReadAt(p []byte, off int64) (int, error) {
	rr.mu.Lock()
	rr.calls = append(rr.calls, readCall{off, off + int64(len(p))})
	rr.mu.Unlock()
	return rr.r.ReadAt(p, off)
}

func (rr *rangeRecorder) snapshot() []readCall {
	rr.mu.Lock()
	defer rr.mu.Unlock()
	return append([]readCall(nil), rr.calls...)
}

func (rr *rangeRecorder) reset() {
	rr.mu.Lock()
	rr.calls = nil
	rr.mu.Unlock()
}

func TestIndexBuild(t *testing.T) {
	ar := buildArchive(t)

	t.Run("happy path and block metadata", func(t *testing.T) {
		var st rleStats
		idx, err := buildIndex(bytes.NewReader(ar.compressed), int64(len(ar.compressed)), &st)
		if err != nil {
			t.Fatalf("BuildIndex: %v", err)
		}
		if idx.Size() != int64(len(ar.compressed)) {
			t.Errorf("Size = %d, want %d", idx.Size(), len(ar.compressed))
		}
		blocks := idx.Blocks()
		if len(blocks) < 3 {
			t.Fatalf("expected several blocks, got %d", len(blocks))
		}

		// Empty streams still consume stream numbers.
		streamSet := map[int]bool{}
		var rawTotal int64
		for i, b := range blocks {
			streamSet[b.Stream] = true
			if b.Level < BestSpeed || b.Level > BestCompression {
				t.Errorf("block %d: invalid level %d", i, b.Level)
			}
			if b.CompressedBitOffset < 0 || b.CompressedBitLength <= 0 {
				t.Errorf("block %d: bad compressed range %d+%d", i, b.CompressedBitOffset, b.CompressedBitLength)
			}
			if b.RawSize <= 0 {
				t.Errorf("block %d: non-positive raw size", i)
			}
			if b.RawOffset != rawTotal {
				t.Errorf("block %d: RawOffset=%d, want %d", i, b.RawOffset, rawTotal)
			}
			rawTotal += b.RawSize

			// The 48 bits at the recorded offset must be the block magic.
			if v := readBits48(ar.compressed, b.CompressedBitOffset); v != blkMagic {
				t.Errorf("block %d: magic at offset %d is %x", i, b.CompressedBitOffset, v)
			}
			// The recorded range must fit inside the input.
			endByte := (b.CompressedBitOffset + b.CompressedBitLength + 7) / 8
			if endByte > int64(len(ar.compressed)) {
				t.Errorf("block %d: range exceeds input", i)
			}
			if i > 0 {
				prev := blocks[i-1]
				if b.CompressedBitOffset < prev.CompressedBitOffset+prev.CompressedBitLength {
					t.Errorf("block %d: compressed ranges overlap", i)
				}
				if b.RawOffset < prev.RawOffset+prev.RawSize {
					t.Errorf("block %d: raw ranges overlap", i)
				}
			}
		}
		if rawTotal != int64(len(ar.plain)) {
			t.Errorf("raw total = %d, want %d", rawTotal, len(ar.plain))
		}
		// Levels 1,9,1,6,2,9 with empties at positions 1 and 4.
		if !streamSet[0] || !streamSet[2] || !streamSet[3] || !streamSet[5] {
			t.Errorf("missing expected streams, got %v", streamSet)
		}
		if got := blocks[len(blocks)-1].Stream; got != 5 {
			t.Errorf("last block stream = %d, want 5", got)
		}

		// The long fill member is one block: its whole run must have been
		// processed as run segments, never as a per-byte loop.
		var fillBlock *BlockInfo
		for i := range blocks {
			if blocks[i].Stream == 2 {
				fillBlock = &blocks[i]
				break
			}
		}
		if fillBlock == nil {
			t.Fatal("long-fill stream produced no blocks")
		}
		if fillBlock.RawSize != 1200000 {
			t.Errorf("fill block size = %d, want 1200000", fillBlock.RawSize)
		}
		// The encoder caps each count byte at 255, so the fill is a chain of
		// run segments separated by four-byte anchors. The repeated fill value
		// must overwhelmingly travel as run segments, not literal bytes.
		if st.RunBytes < 1000000 {
			t.Errorf("only %d of the 1200000 fill bytes traveled as runs", st.RunBytes)
		}
		if st.RunSegments == 0 {
			t.Error("no run segments recorded for the fill")
		}
	})

	t.Run("Blocks returns a mutable copy", func(t *testing.T) {
		idx, err := BuildIndex(bytes.NewReader(ar.compressed), int64(len(ar.compressed)))
		if err != nil {
			t.Fatalf("BuildIndex: %v", err)
		}
		b1 := idx.Blocks()
		if len(b1) == 0 {
			t.Fatal("no blocks")
		}
		origCRC, origSize := b1[0].CRC, b1[0].RawSize
		b1[0].CRC ^= 0xdeadbeef
		b1[0].RawSize = -1
		b2 := idx.Blocks()
		if b2[0].CRC != origCRC || b2[0].RawSize != origSize {
			t.Fatal("mutating the Blocks() copy changed the index")
		}
	})

	t.Run("single empty stream and empty between streams", func(t *testing.T) {
		empty := compressMember(t, nil, 6)
		idx, err := BuildIndex(bytes.NewReader(empty), int64(len(empty)))
		if err != nil {
			t.Fatalf("BuildIndex on empty stream: %v", err)
		}
		if len(idx.Blocks()) != 0 {
			t.Fatalf("empty stream produced blocks: %d", len(idx.Blocks()))
		}
		if idx.rawSize() != 0 {
			t.Fatalf("empty stream raw size = %d", idx.rawSize())
		}

		// Empty stream in the middle keeps its stream number.
		first := compressMember(t, []byte("aaaa"), 1)
		mid := compressMember(t, nil, 9)
		last := compressMember(t, []byte("bbbb"), 1)
		multi := bytes.Join([][]byte{first, mid, last}, nil)
		idx2, err := BuildIndex(bytes.NewReader(multi), int64(len(multi)))
		if err != nil {
			t.Fatalf("BuildIndex on split streams: %v", err)
		}
		bs := idx2.Blocks()
		if len(bs) != 2 || bs[0].Stream != 0 || bs[1].Stream != 2 {
			t.Fatalf("stream numbering wrong: %+v", bs)
		}
	})

	t.Run("all compression levels", func(t *testing.T) {
		var buf bytes.Buffer
		for lvl := BestSpeed; lvl <= BestCompression; lvl++ {
			buf.Write(compressMember(t, pseudoRandom(5000+lvl), lvl))
		}
		idx, err := BuildIndex(bytes.NewReader(buf.Bytes()), int64(buf.Len()))
		if err != nil {
			t.Fatalf("BuildIndex: %v", err)
		}
		seen := map[int]bool{}
		for _, b := range idx.Blocks() {
			seen[b.Level] = true
		}
		for lvl := BestSpeed; lvl <= BestCompression; lvl++ {
			if !seen[lvl] {
				t.Errorf("level %d not represented", lvl)
			}
		}
	})

	t.Run("rejects malformed input", func(t *testing.T) {
		src := bytes.NewReader(ar.compressed)
		if idx, err := BuildIndex(src, -1); err == nil || idx != nil {
			t.Errorf("negative size: got (%v, %v)", idx, err)
		}
		if idx, err := BuildIndex(src, 0); err == nil || idx != nil {
			t.Errorf("empty input: got (%v, %v)", idx, err)
		}
		if idx, err := BuildIndex(nil, 1); err == nil || idx != nil {
			t.Errorf("nil source: got (%v, %v)", idx, err)
		}

		cases := []struct {
			name string
			data []byte
		}{
			{"truncated", ar.compressed[:len(ar.compressed)-7]},
			{"trailing garbage", append(bytes.Clone(ar.compressed), 0x42)},
			{"bad header", []byte("XXh9........")},
		}
		for _, c := range cases {
			if idx, err := BuildIndex(bytes.NewReader(c.data), int64(len(c.data))); err == nil || idx != nil {
				t.Errorf("%s: got (%v, %v), want error", c.name, idx, err)
			}
		}
	})

	t.Run("block and stream CRC failures fail at distinct stages", func(t *testing.T) {
		idx, err := BuildIndex(bytes.NewReader(ar.compressed), int64(len(ar.compressed)))
		if err != nil {
			t.Fatalf("BuildIndex: %v", err)
		}
		blocks := idx.Blocks()

		// Corrupt the stored CRC of the first block (right after its magic).
		badBlock := bytes.Clone(ar.compressed)
		flipBit(badBlock, blocks[0].CompressedBitOffset+48)
		_, err = BuildIndex(bytes.NewReader(badBlock), int64(len(badBlock)))
		if err == nil {
			t.Fatal("corrupted block CRC accepted")
		}
		if !strings.Contains(err.Error(), "block checksum") {
			t.Errorf("block corruption failed with %q, want block checksum error", err.Error())
		}

		// Corrupt the stream footer CRC. The footer magic starts immediately
		// after the final block's bit range.
		last := blocks[len(blocks)-1]
		footerMagicBit := last.CompressedBitOffset + last.CompressedBitLength
		badStream := bytes.Clone(ar.compressed)
		// Skip the 48-bit footer magic, flip one bit of its 32-bit CRC.
		flipBit(badStream, footerMagicBit+48+3)
		// Sanity check: the footer magic really sits at that offset.
		if v := readBits48(badStream, footerMagicBit); v != endMagic {
			t.Fatalf("test setup: footer magic not at %d (%x)", footerMagicBit, v)
		}
		_, err = BuildIndex(bytes.NewReader(badStream), int64(len(badStream)))
		if err == nil {
			t.Fatal("corrupted stream CRC accepted")
		}
		if !strings.Contains(err.Error(), "stream checksum") {
			t.Errorf("stream corruption failed with %q, want stream checksum error", err.Error())
		}
	})

	t.Run("RLE1 run CRC matches the bytewise CRC", func(t *testing.T) {
		// Encoded RLE1 buffers covering short runs, adjacent runs of the same
		// value, maximum repeater bytes, and literal block tails.
		encoded := [][]byte{
			{},
			{0},
			{'a', 'a', 'a', 'a', 0},
			{'a', 'a', 'a', 'a', 251},      // 255 a's
			{'a', 'a', 'a', 'a', 251, 'a'}, // 256 a's
			{'a', 'a', 'a', 'a', 251, 'a', 'a', 'a', 'a', 0},   // 259 a's, adjacent same-value runs
			{'a', 'a', 'a', 'a', 251, 'a', 'a', 'a', 'a', 241}, // 500 a's
			append(append([]byte("head"), bytes.Repeat([]byte{0xff, 0xff, 0xff, 0xff, 0}, 4)...), 'x', 'y'),
		}
		for i, enc := range encoded {
			// Reference expansion through the production RLE1 decoder.
			var rle runLengthEncoding
			rle.Init(enc)
			var plain []byte
			scratch := make([]byte, 7)
			for {
				n, rerr := rle.Read(scratch)
				plain = append(plain, scratch[:n]...)
				if rerr == rleDone {
					break
				}
				if rerr != nil {
					t.Fatalf("case %d: reference RLE1: %v", i, rerr)
				}
			}
			var bytewise, runwise crc
			bytewise.update(plain)

			var stream rle1Stream
			stream.buf = enc
			var total int64
			for {
				seg, ok := stream.next()
				if !ok {
					break
				}
				if seg.lit != nil {
					runwise.update(seg.lit)
					total += int64(len(seg.lit))
				} else {
					runwise.updateRun(seg.val, seg.cnt)
					total += seg.cnt
				}
			}
			if total != int64(len(plain)) {
				t.Errorf("case %d: expanded %d bytes, want %d", i, total, len(plain))
			}
			if runwise.val != bytewise.val {
				t.Errorf("case %d: run CRC %08x != bytewise CRC %08x", i, runwise.val, bytewise.val)
			}
		}

		// updateRun must stay O(log n): assert the step count on a long run.
		var c crc
		steps := c.updateRun(0x5a, 1<<30)
		if steps > 30 {
			t.Errorf("updateRun took %d doubling steps for 1<<30 bytes, want <= 30", steps)
		}
	})

	t.Run("foreign reader vectors", func(t *testing.T) {
		for _, v := range readerVectors {
			if len(v.input) == 0 {
				if idx, err := BuildIndex(bytes.NewReader(v.input), 0); err == nil || idx != nil {
					t.Errorf("%s: empty input should fail", v.name)
				}
				continue
			}
			idx, err := BuildIndex(bytes.NewReader(v.input), int64(len(v.input)))
			if v.errf != "" {
				if err == nil {
					t.Errorf("%s: invalid vector accepted", v.name)
				}
				continue
			}
			if err != nil {
				t.Errorf("%s: BuildIndex: %v", v.name, err)
				continue
			}
			if idx.rawSize() != int64(len(v.output)) {
				t.Errorf("%s: raw size %d, want %d", v.name, idx.rawSize(), len(v.output))
			}
			ir, err := NewIndexedReader(bytes.NewReader(v.input), idx)
			if err != nil {
				t.Errorf("%s: NewIndexedReader: %v", v.name, err)
				continue
			}
			got := make([]byte, len(v.output))
			if n, err := ir.ReadAt(got, 0); n != len(got) || err != nil {
				t.Errorf("%s: ReadAt = (%d, %v)", v.name, n, err)
				continue
			}
			if !bytes.Equal(got, v.output) {
				t.Errorf("%s: output mismatch", v.name)
			}
		}
	})
}

func TestIndexReadAt(t *testing.T) {
	ar := buildArchive(t)
	idx, err := BuildIndex(bytes.NewReader(ar.compressed), int64(len(ar.compressed)))
	if err != nil {
		t.Fatalf("BuildIndex: %v", err)
	}

	t.Run("full and windowed reads", func(t *testing.T) {
		ir, err := NewIndexedReader(bytes.NewReader(ar.compressed), idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}

		// Whole archive in one call.
		full := make([]byte, len(ar.plain))
		if n, err := ir.ReadAt(full, 0); n != len(full) || err != nil {
			t.Fatalf("full ReadAt = (%d, %v)", n, err)
		}
		if !bytes.Equal(full, ar.plain) {
			t.Fatal("full decompression mismatch")
		}

		// Deterministic scatter of offsets and lengths, including reads that
		// straddle block and stream boundaries.
		r := rand.New(rand.NewSource(7))
		for iter := 0; iter < 300; iter++ {
			off := r.Int63n(int64(len(ar.plain)) + 1)
			n := r.Intn(4096) + 1
			p := make([]byte, n)
			got, gerr := ir.ReadAt(p, off)
			want := ar.plain[off:]
			if int64(n) > int64(len(want)) {
				if gerr != io.EOF {
					t.Errorf("iter %d: past-end err = %v, want EOF", iter, gerr)
				}
				if int64(got) != int64(len(want)) {
					t.Errorf("iter %d: past-end n = %d, want %d", iter, got, len(want))
				}
				p = p[:got]
			} else if gerr != nil {
				t.Errorf("iter %d: unexpected err %v", iter, gerr)
				continue
			}
			if !bytes.Equal(p, want[:len(p)]) {
				t.Fatalf("iter %d (off=%d,n=%d): content mismatch", iter, off, n)
			}
		}
	})

	t.Run("ReaderAt length conventions", func(t *testing.T) {
		counting := &countingReaderAt{r: bytes.NewReader(ar.compressed)}
		ir, err := NewIndexedReader(counting, idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}

		if n, err := ir.ReadAt(make([]byte, 1), int64(len(ar.plain))); n != 0 || err != io.EOF {
			t.Errorf("read at end = (%d, %v), want (0, EOF)", n, err)
		}
		if n, err := ir.ReadAt(make([]byte, 1), int64(len(ar.plain))+100); n != 0 || err != io.EOF {
			t.Errorf("read past end = (%d, %v), want (0, EOF)", n, err)
		}
		before := counting.calls
		if n, err := ir.ReadAt(nil, 123); n != 0 || err != nil {
			t.Errorf("zero-length read = (%d, %v)", n, err)
		}
		if counting.calls != before {
			t.Errorf("zero-length read touched the source %d times", counting.calls-before)
		}
		if _, err := ir.ReadAt(make([]byte, 1), -1); !cerr.IsInvalid(err) {
			t.Errorf("negative offset err = %v, want invalid", err)
		}

		// Partial read past the end returns the copied count plus EOF.
		p := make([]byte, 16)
		n, err := ir.ReadAt(p, int64(len(ar.plain))-4)
		if n != 4 || err != io.EOF {
			t.Errorf("tail read = (%d, %v), want (4, EOF)", n, err)
		}
		if !bytes.Equal(p[:4], ar.plain[len(ar.plain)-4:]) {
			t.Error("tail content mismatch")
		}
	})

	t.Run("constructor rejects nil arguments and never reads the source", func(t *testing.T) {
		if _, err := NewIndexedReader(nil, idx); err == nil {
			t.Error("nil ReaderAt accepted")
		}
		if _, err := NewIndexedReader(bytes.NewReader(ar.compressed), nil); err == nil {
			t.Error("nil Index accepted")
		}
		rec := &rangeRecorder{r: bytes.NewReader(ar.compressed)}
		if _, err := NewIndexedReader(rec, idx); err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		if calls := rec.snapshot(); len(calls) != 0 {
			t.Errorf("constructor read the source %d times", len(calls))
		}
	})

	t.Run("empty-stream index behaves like zero-length data", func(t *testing.T) {
		empty := compressMember(t, nil, 6)
		eidx, err := BuildIndex(bytes.NewReader(empty), int64(len(empty)))
		if err != nil {
			t.Fatalf("BuildIndex: %v", err)
		}
		rec := &rangeRecorder{r: bytes.NewReader(empty)}
		eir, err := NewIndexedReader(rec, eidx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		if n, err := eir.ReadAt(make([]byte, 1), 0); n != 0 || err != io.EOF {
			t.Errorf("non-empty read = (%d, %v), want (0, EOF)", n, err)
		}
		if n, err := eir.ReadAt(nil, 0); n != 0 || err != nil {
			t.Errorf("zero-length read = (%d, %v)", n, err)
		}
		if calls := rec.snapshot(); len(calls) != 0 {
			t.Errorf("empty-stream index read the source %d times", len(calls))
		}
	})

	t.Run("suffix reads never fetch prefix bytes", func(t *testing.T) {
		rec := &rangeRecorder{r: bytes.NewReader(ar.compressed)}
		ir, err := NewIndexedReader(rec, idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		blocks := idx.Blocks()

		// Read a fragment from deep inside one specific block.
		target := blocks[len(blocks)/2]
		off := target.RawOffset + target.RawSize/2
		p := make([]byte, 37)
		if n, err := ir.ReadAt(p, off); n != len(p) || err != nil {
			t.Fatalf("ReadAt = (%d, %v)", n, err)
		}
		if !bytes.Equal(p, ar.plain[off:off+37]) {
			t.Fatal("fragment content mismatch")
		}

		firstByte := target.CompressedBitOffset / 8
		lastByte := (target.CompressedBitOffset + target.CompressedBitLength - 1) / 8
		for _, c := range rec.snapshot() {
			if c.off < firstByte {
				t.Errorf("read %d-%d precedes block window starting at %d", c.off, c.end, firstByte)
			}
			if c.end > lastByte+1 {
				t.Errorf("read %d-%d exceeds block window ending at %d", c.off, c.end, lastByte+1)
			}
		}

		// A second, later read must not revisit any byte of the first block.
		rec.reset()
		later := blocks[len(blocks)-1]
		lowerBound := later.CompressedBitOffset / 8
		p2 := make([]byte, 19)
		if n, err := ir.ReadAt(p2, later.RawOffset+1); n != len(p2) || err != nil {
			t.Fatalf("second ReadAt = (%d, %v)", n, err)
		}
		for _, c := range rec.snapshot() {
			if c.end <= lowerBound {
				t.Errorf("suffix read re-fetched prefix byte range %d-%d", c.off, c.end)
			}
		}
	})

	t.Run("each intersected block window is read at most once and never escaped", func(t *testing.T) {
		rec := &rangeRecorder{r: bytes.NewReader(ar.compressed)}
		ir, err := NewIndexedReader(rec, idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		blocks := idx.Blocks()
		// Span three blocks in one call.
		b0, b2 := blocks[1], blocks[3]
		off := b0.RawOffset + 1
		end := b2.RawOffset + b2.RawSize - 1
		p := make([]byte, end-off)
		if n, err := ir.ReadAt(p, off); int64(n) != end-off || err != nil {
			t.Fatalf("ReadAt = (%d, %v)", n, err)
		}

		intersected := blocks[1:4]
		unionLo := b0.CompressedBitOffset / 8
		unionHi := (b2.CompressedBitOffset + b2.CompressedBitLength + 7) / 8

		// Attribute every source call to one block window by its start offset;
		// adjacent blocks may share one boundary byte, but each starts its own
		// fetch sequence at its own first byte.
		perBlock := make([][]readCall, len(intersected))
		for _, c := range rec.snapshot() {
			if c.off < unionLo || c.end > unionHi {
				t.Errorf("read %d-%d escapes the intersected window [%d,%d)", c.off, c.end, unionLo, unionHi)
				continue
			}
			owner := -1
			for bi, b := range intersected {
				if c.off == b.CompressedBitOffset/8 {
					owner = bi
					break
				}
			}
			if owner < 0 {
				t.Errorf("read %d-%d belongs to no intersected block", c.off, c.end)
				continue
			}
			perBlock[owner] = append(perBlock[owner], c)
		}
		// Within each block its own ranges must be disjoint: a block is never
		// decoded twice, even though adjacent blocks share one boundary byte.
		for bi, cs := range perBlock {
			if len(cs) == 0 {
				t.Errorf("block %d was never read", bi)
			}
			for i := range cs {
				for j := i + 1; j < len(cs); j++ {
					if cs[j].off < cs[i].end && cs[j].end > cs[i].off {
						t.Errorf("block %d read twice: %d-%d overlaps %d-%d", bi, cs[i].off, cs[i].end, cs[j].off, cs[j].end)
					}
				}
			}
			// The block window itself is fully fetched, within its own bounds.
			b := intersected[bi]
			firstByte := b.CompressedBitOffset / 8
			lastByte := (b.CompressedBitOffset + b.CompressedBitLength - 1) / 8
			var covered int64
			for _, c := range cs {
				if c.off < firstByte || c.end > lastByte+1 {
					t.Errorf("block %d read %d-%d leaves its window [%d,%d)", bi, c.off, c.end, firstByte, lastByte+1)
				}
				covered += c.end - c.off
			}
			if covered != lastByte+1-firstByte {
				t.Errorf("block %d covered %d bytes, want window of %d", bi, covered, lastByte+1-firstByte)
			}
		}
	})

	t.Run("short underlying reads and source errors", func(t *testing.T) {
		short := &shortReaderAt{r: bytes.NewReader(ar.compressed), max: 5}
		ir, err := NewIndexedReader(short, idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		if _, err := ir.ReadAt(make([]byte, 16), 0); err == nil {
			t.Error("short source read accepted")
		}

		sentinel := stderrors.New("sentinel source failure")
		fail := &failReaderAt{r: bytes.NewReader(ar.compressed), failAt: 4, err: sentinel}
		ir2, err := NewIndexedReader(fail, idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		_, err = ir2.ReadAt(make([]byte, 16), 0)
		if !stderrors.Is(err, sentinel) {
			t.Errorf("source error not preserved: %v", err)
		}
	})

	t.Run("corrupted block publishes nothing from the failing block", func(t *testing.T) {
		blocks := idx.Blocks()
		target := blocks[1]
		bad := bytes.Clone(ar.compressed)
		// Flip a byte well inside the block's payload, past the 10-byte
		// header so the stored block CRC is the thing that fails.
		firstByte := target.CompressedBitOffset / 8
		lastByte := (target.CompressedBitOffset + target.CompressedBitLength - 1) / 8
		payloadByte := firstByte + (lastByte-firstByte)/2 + 4
		bad[payloadByte] ^= 0x3c

		ir, err := NewIndexedReader(bytes.NewReader(bad), idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		// Request spanning the good first block and the damaged second one.
		off := blocks[0].RawOffset + blocks[0].RawSize/2
		p := bytes.Repeat([]byte{0x7e}, int(blocks[1].RawOffset+blocks[1].RawSize-off))
		want := bytes.Clone(p)
		n, err := ir.ReadAt(p, off)
		if err == nil {
			t.Fatal("damaged block accepted")
		}
		goodFragment := int(blocks[0].RawOffset + blocks[0].RawSize - off)
		if n != goodFragment {
			t.Errorf("n = %d, want only the verified first-block fragment %d", n, goodFragment)
		}
		// The failing block's region must be byte-for-byte unchanged.
		if !bytes.Equal(p[goodFragment:], want[goodFragment:]) {
			t.Error("bytes of the failing block were published")
		}
		// The verified fragment was published and is correct.
		if !bytes.Equal(p[:goodFragment], ar.plain[off:off+int64(goodFragment)]) {
			t.Error("verified fragment content mismatch")
		}

		// The same corruption must be reported when the request starts inside
		// the damaged block itself.
		p2 := bytes.Repeat([]byte{0x11}, 64)
		want2 := bytes.Clone(p2)
		n2, err2 := ir.ReadAt(p2, blocks[1].RawOffset+3)
		if n2 != 0 || err2 == nil {
			t.Errorf("failing-block read = (%d, %v), want (0, error)", n2, err2)
		}
		if !bytes.Equal(p2, want2) {
			t.Error("failing block published bytes on a local request")
		}
	})

	t.Run("long RLE1 fill stays a run on random reads", func(t *testing.T) {
		// A dedicated small archive: one block whose output is a huge run.
		plain := bytes.Repeat([]byte{'Z'}, 1200000)
		compressed := compressMember(t, plain, 1)
		var bst rleStats
		bidx, err := buildIndex(bytes.NewReader(compressed), int64(len(compressed)), &bst)
		if err != nil {
			t.Fatalf("BuildIndex: %v", err)
		}
		if bst.RunSegments == 0 || bst.RunBytes+bst.LitBytes != 1200000 {
			t.Errorf("build run accounting: segments=%d run=%d lit=%d", bst.RunSegments, bst.RunBytes, bst.LitBytes)
		}
		ir, err := NewIndexedReader(bytes.NewReader(compressed), bidx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		var rst rleStats
		// Read a tiny window from the middle of the block; the whole block is
		// still validated as runs, not expanded into a giant slice.
		p := make([]byte, 64)
		if n, err := ir.readAt(p, 700000, &rst); n != len(p) || err != nil {
			t.Fatalf("readAt = (%d, %v)", n, err)
		}
		if rst.RunBytes+rst.LitBytes != 1200000 {
			t.Errorf("read validated %d run + %d literal bytes, want 1200000 total", rst.RunBytes, rst.LitBytes)
		}
		if rst.CRCSteps >= 120000 {
			t.Errorf("read CRC work %d steps scales with run length", rst.CRCSteps)
		}
		if !bytes.Equal(p, bytes.Repeat([]byte{'Z'}, 64)) {
			t.Error("run fragment content mismatch")
		}
	})
}

func TestIndexIsolation(t *testing.T) {
	ar := buildArchive(t)
	idx, err := BuildIndex(bytes.NewReader(ar.compressed), int64(len(ar.compressed)))
	if err != nil {
		t.Fatalf("BuildIndex: %v", err)
	}

	t.Run("concurrent reads on one IndexedReader", func(t *testing.T) {
		ir, err := NewIndexedReader(bytes.NewReader(ar.compressed), idx)
		if err != nil {
			t.Fatalf("NewIndexedReader: %v", err)
		}
		var wg sync.WaitGroup
		for g := 0; g < 16; g++ {
			wg.Add(1)
			go func(seed int64) {
				defer wg.Done()
				r := rand.New(rand.NewSource(seed))
				for i := 0; i < 50; i++ {
					off := r.Int63n(int64(len(ar.plain)))
					n := r.Intn(1024) + 1
					if off+int64(n) > int64(len(ar.plain)) {
						n = int(int64(len(ar.plain)) - off)
					}
					p := make([]byte, n)
					got, gerr := ir.ReadAt(p, off)
					if gerr != nil || got != n {
						t.Errorf("read (off=%d,n=%d) = (%d,%v)", off, n, got, gerr)
						return
					}
					if !bytes.Equal(p, ar.plain[off:off+int64(n)]) {
						t.Errorf("content mismatch at off=%d", off)
						return
					}
				}
			}(int64(g) + 1)
		}
		wg.Wait()
	})

	t.Run("two readers share one index independently", func(t *testing.T) {
		ir1, err := NewIndexedReader(bytes.NewReader(ar.compressed), idx)
		if err != nil {
			t.Fatalf("NewIndexedReader ir1: %v", err)
		}
		ir2, err := NewIndexedReader(bytes.NewReader(ar.compressed), idx)
		if err != nil {
			t.Fatalf("NewIndexedReader ir2: %v", err)
		}
		// Interleave reads; neither reader keeps a cursor, so outputs must
		// depend solely on the explicit offsets.
		r := rand.New(rand.NewSource(99))
		for i := 0; i < 100; i++ {
			off := r.Int63n(int64(len(ar.plain)) - 256)
			p1 := make([]byte, 256)
			p2 := make([]byte, 256)
			n1, e1 := ir1.ReadAt(p1, off)
			n2, e2 := ir2.ReadAt(p2, off+1)
			if e1 != nil || n1 != 256 || e2 != nil || n2 != 256 {
				t.Fatalf("read %d: (%d,%v) (%d,%v)", i, n1, e1, n2, e2)
			}
			if !bytes.Equal(p1, ar.plain[off:off+256]) || !bytes.Equal(p2, ar.plain[off+1:off+257]) {
				t.Fatalf("shared-index readers diverged at iter %d", i)
			}
		}
	})

	t.Run("index is unaffected by readers", func(t *testing.T) {
		before := idx.Blocks()
		ir, _ := NewIndexedReader(bytes.NewReader(ar.compressed), idx)
		for i := 0; i < 20; i++ {
			p := make([]byte, 128)
			if _, err := ir.ReadAt(p, int64(i*137)); err != nil && err != io.EOF {
				t.Fatalf("ReadAt: %v", err)
			}
		}
		after := idx.Blocks()
		if len(before) != len(after) {
			t.Fatal("block count changed after reads")
		}
		for i := range before {
			if before[i] != after[i] {
				t.Fatalf("block %d changed after reads", i)
			}
		}
	})
}

// readBits48 extracts 48 big-endian-ordered bits starting at bit offset off.
func readBits48(d []byte, off int64) uint64 {
	var v uint64
	for i := int64(0); i < 48; i++ {
		bit := int64(d[(off+i)/8]>>(7-uint((off+i)%8))) & 1
		v = v<<1 | uint64(bit)
	}
	return v
}

// countingReaderAt counts ReadAt calls.
type countingReaderAt struct {
	r     io.ReaderAt
	calls int
}

func (c *countingReaderAt) ReadAt(p []byte, off int64) (int, error) {
	c.calls++
	return c.r.ReadAt(p, off)
}

// shortReaderAt always returns fewer bytes than requested without EOF.
type shortReaderAt struct {
	r   io.ReaderAt
	max int
}

func (s *shortReaderAt) ReadAt(p []byte, off int64) (int, error) {
	if len(p) > s.max {
		p = p[:s.max]
	}
	n, err := s.r.ReadAt(p, off)
	if err == io.EOF && n > 0 {
		err = nil
	}
	return n, err
}

// failReaderAt returns err for any read at or past failAt.
type failReaderAt struct {
	r      io.ReaderAt
	failAt int64
	err    error
}

func (f *failReaderAt) ReadAt(p []byte, off int64) (int, error) {
	if off >= f.failAt {
		return 0, f.err
	}
	if off+int64(len(p)) > f.failAt {
		p = p[:f.failAt-off]
	}
	return f.r.ReadAt(p, off)
}
