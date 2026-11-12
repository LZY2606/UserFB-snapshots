// Copyright 2015, Joe Tsai. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.md file.

package bzip2

import (
	"io"
	"sync"

	"github.com/dsnet/compress/internal/errors"
)

// BlockInfo describes one compressed bzip2 block and its location in both the
// compressed input and the fully decompressed output.
//
// CompressedBitOffset and CompressedBitLength are measured in bits starting
// from the most-significant bit of the first input byte. The covered range
// includes the block magic and the block header, and ends immediately after
// the block's Huffman end-of-block symbol; it neither includes the next block
// nor the stream footer.
//
// RawOffset and RawSize are the half-open byte interval of the block's
// decompressed data within the concatenated output of the whole archive.
type BlockInfo struct {
	Stream              int    // Zero-based index of the containing bzip2 stream
	Level               int    // Compression level from the stream's BZh header
	CompressedBitOffset int64  // Bit offset of the block magic
	CompressedBitLength int64  // Number of bits in the block
	RawOffset           int64  // Byte offset of the block data in the output
	RawSize             int64  // Number of decompressed bytes in the block
	CRC                 uint32 // CRC-32 of the block's decompressed data
}

// Index is a reusable, random-access index over one or more concatenated
// bzip2 streams. It only stores per-block metadata, so its size is
// proportional to the number of blocks rather than the decompressed size.
//
// Building an index requires one sequential, fully verified decode of the
// input: every block is expanded through RLE1 and every block and stream
// checksum is checked. Once built, the index is immutable and may be shared
// by any number of IndexedReader instances and concurrent ReadAt calls.
//
// An Index does not detect later modifications to regions of the source that
// no ReadAt call has touched. Random reads only re-verify the blocks they
// actually intersect; blocks outside the requested range are not re-checked.
type Index struct {
	compressedSize int64
	blocks         []BlockInfo
}

// Size returns the total number of compressed bytes covered by the index,
// equal to the compressedSize passed to BuildIndex.
func (idx *Index) Size() int64 {
	return idx.compressedSize
}

// Blocks returns a copy of the indexed block table. Mutating the returned
// slice has no effect on the index.
func (idx *Index) Blocks() []BlockInfo {
	out := make([]BlockInfo, len(idx.blocks))
	copy(out, idx.blocks)
	return out
}

// rangeReaderAt adapts an io.ReaderAt to the io.Reader interface while
// guaranteeing that every read stays inside one byte-aligned window
// [base, limit). Errors from the underlying ReaderAt are passed through
// untouched so that errors.Is keeps recognizing them.
type rangeReaderAt struct {
	r     io.ReaderAt
	base  int64 // Absolute byte offset of position zero
	limit int64 // Absolute byte offset just past the allowed window
	pos   int64 // Bytes read since base
}

func (rr *rangeReaderAt) Read(p []byte) (int, error) {
	off := rr.base + rr.pos
	if off >= rr.limit {
		return 0, io.EOF
	}
	n := int64(len(p))
	if max := rr.limit - off; n > max {
		n = max
	}
	cnt, err := rr.r.ReadAt(p[:n], off)
	rr.pos += int64(cnt)
	if err == io.EOF && int64(cnt) == n {
		// A full read alongside EOF is a successful read.
		err = nil
	}
	if err == nil && cnt < int(n) {
		err = io.ErrUnexpectedEOF
	}
	return cnt, err
}

// BuildIndex sequentially decodes r over the half-open compressed interval
// [0, compressedSize) and builds a block index. Block boundaries are found by
// genuinely parsing the bzip2 format (never by scanning for magic values), and
// an index is only returned after every block CRC, every concatenated-stream
// CRC, and every stream footer has been verified.
//
// Empty streams consume a stream number but produce no block entries.
// Negative sizes, empty input, truncation, corrupted blocks or footers, and
// trailing garbage after the final stream all yield a nil index and an error.
func BuildIndex(r io.ReaderAt, compressedSize int64) (*Index, error) {
	return buildIndex(r, compressedSize, nil)
}

func buildIndex(r io.ReaderAt, compressedSize int64, st *rleStats) (_ *Index, err error) {
	if r == nil {
		return nil, errorf(errors.Invalid, "nil ReaderAt")
	}
	if compressedSize < 0 {
		return nil, errorf(errors.Invalid, "negative compressed size: %d", compressedSize)
	}
	if compressedSize == 0 {
		return nil, errorf(errors.Corrupted, "empty bzip2 input")
	}

	idx := &Index{compressedSize: compressedSize}
	var dec blockDecoder
	win := &rangeReaderAt{r: r, base: 0, limit: compressedSize}
	dec.rd.Init(win)

	var (
		streamNum  int
		level      int
		rawOffset  int64
		endCRC     uint32
		streamSeen bool
	)

	for {
		// Read the stream header.
		func() {
			defer errors.Recover(&err)
			// Check whether another stream starts here.
			if e := dec.rd.PullBits(1); e != nil {
				if e == io.ErrUnexpectedEOF && streamSeen {
					err = io.EOF // Clean end exactly at the compressed boundary.
					return
				}
				errors.Panic(e)
			}
			if dec.rd.ReadBitsBE64(16) != hdrMagic {
				panicf(errors.Corrupted, "invalid stream magic")
			}
			if ver := dec.rd.ReadBitsBE64(8); ver != 'h' {
				if ver == '0' {
					panicf(errors.Deprecated, "bzip1 format is not supported")
				}
				panicf(errors.Corrupted, "invalid version: %q", ver)
			}
			lvl := int(dec.rd.ReadBitsBE64(8)) - '0'
			if lvl < BestSpeed || lvl > BestCompression {
				panicf(errors.Corrupted, "invalid block size: %d", lvl*blockSize)
			}
			level = lvl
		}()
		if err == io.EOF {
			return idx, nil
		}
		if err != nil {
			return nil, errWrap(err, errors.Corrupted)
		}
		streamSeen = true
		endCRC = 0

		// Decode every block and the footer of this stream.
		streamEnd := false
		for !streamEnd {
			func() {
				defer errors.Recover(&err)
				magic := dec.rd.ReadBitsBE64(48)
				if magic == endMagic {
					want := uint32(dec.rd.ReadBitsBE64(32))
					if endCRC != want {
						panicf(errors.Corrupted, "mismatching stream checksum")
					}
					dec.rd.ReadPads()
					streamEnd = true
					return
				}
				if magic != blkMagic {
					panicf(errors.Corrupted, "invalid block or footer magic")
				}
				blockStart := dec.rd.BitsRead() - 48
				buf, blkCRC := dec.decodeBlock(level)
				blockEndBits := dec.rd.BitsRead()

				// RLE1 expansion determines the real output length; long runs
				// are counted and CRC-checked without materializing them.
				n, gotCRC := dec.drainRLE1(buf, nil, 0, 0, st)
				if gotCRC != blkCRC {
					panicf(errors.Corrupted, "mismatching block checksum")
				}
				endCRC = (endCRC<<1 | endCRC>>31) ^ blkCRC

				idx.blocks = append(idx.blocks, BlockInfo{
					Stream:              streamNum,
					Level:               level,
					CompressedBitOffset: blockStart,
					CompressedBitLength: blockEndBits - blockStart,
					RawOffset:           rawOffset,
					RawSize:             n,
					CRC:                 blkCRC,
				})
				rawOffset += n
			}()
			if err != nil {
				return nil, errWrap(err, errors.Corrupted)
			}
		}

		streamNum++
		// Commit buffered bytes so the next stream header starts on a clean
		// reader. Any trailing bytes remain visible to PullBits below.
		if _, ferr := dec.rd.Flush(); ferr != nil && ferr != io.EOF {
			return nil, errWrap(ferr, errors.Corrupted)
		}
	}
}

// IndexedReader provides random access to the decompressed bytes of an
// indexed bzip2 archive. Each ReadAt re-decodes only the blocks that overlap
// the requested range, and a block is fully expanded and checksum-verified
// before any of its bytes are published to the caller's buffer.
//
// IndexedReader holds no sequential cursor and keeps no mutable decode state
// between calls, so one instance may serve concurrent ReadAt calls; multiple
// readers may also share one Index. The underlying io.ReaderAt must itself be
// safe for concurrent use.
type IndexedReader struct {
	r   io.ReaderAt
	idx *Index
}

// NewIndexedReader creates a random-access reader. It does not read from r;
// the index already carries everything needed to locate blocks. Both
// arguments are required.
func NewIndexedReader(r io.ReaderAt, idx *Index) (*IndexedReader, error) {
	if r == nil {
		return nil, errorf(errors.Invalid, "nil ReaderAt")
	}
	if idx == nil {
		return nil, errorf(errors.Invalid, "nil Index")
	}
	return &IndexedReader{r: r, idx: idx}, nil
}

// ReadAt reads decompressed bytes starting at byte offset off into p. It
// implements the io.ReaderAt length conventions:
//
//   - A fully satisfied request returns len(p), nil.
//   - A request extending past the end of the decompressed data returns the
//     number of bytes copied and io.EOF.
//   - A non-empty request starting at or past the end returns 0, io.EOF.
//   - A negative offset returns an error without modifying p.
//   - A zero-length request with a non-negative offset returns 0, nil and
//     never touches the underlying source.
//
// Every intersected block is decoded in full and validated against the length
// and CRC recorded in the index before its requested fragment is written to
// p. If a block fails, bytes from earlier, already verified blocks are still
// counted in n; the failing block and everything after it leave p untouched.
func (ir *IndexedReader) ReadAt(p []byte, off int64) (n int, err error) {
	return ir.readAt(p, off, nil)
}

func (ir *IndexedReader) readAt(p []byte, off int64, st *rleStats) (n int, err error) {
	if off < 0 {
		return 0, errorf(errors.Invalid, "negative offset: %d", off)
	}
	if len(p) == 0 {
		return 0, nil
	}

	total := ir.idx.rawSize()
	if off >= total {
		return 0, io.EOF
	}
	wantEnd := off + int64(len(p))
	if wantEnd > total {
		wantEnd = total
	}

	blocks := ir.idx.blocks
	for bi := blockSearch(blocks, off); bi < len(blocks); bi++ {
		b := blocks[bi]
		if b.RawOffset >= wantEnd {
			break
		}
		if b.RawOffset+b.RawSize <= off {
			continue
		}

		// Fragment of this block that belongs to the request.
		rLo := max64(off-b.RawOffset, 0)
		rHi := min64(wantEnd-b.RawOffset, b.RawSize)
		pLo := max64(b.RawOffset-off, 0)
		pHi := pLo + (rHi - rLo)

		cnt, derr := ir.decodeBlockRange(b, p[pLo:pHi], rLo, rHi, st)
		if derr != nil {
			// The block failed verification; its bytes were never written to
			// p, so p beyond the already confirmed blocks stays untouched. A
			// corruption must surface distinctly from the normal end EOF.
			return n, derr
		}
		n += cnt
	}

	if int64(n) != wantEnd-off {
		return n, io.EOF
	}
	if wantEnd < off+int64(len(p)) {
		return n, io.EOF // The request ran past the logical end.
	}
	return n, nil
}

// decPool hands out independent block-decoding workspaces. The workspaces
// themselves are not safe to share, so every concurrent ReadAt, and every
// block within one ReadAt, gets its own; returning them to the pool keeps the
// repeated BWT allocations out of the hot path without caching block output.
var decPool = sync.Pool{
	New: func() any { return new(blockDecoder) },
}

// decodeBlockRange decodes one indexed block in isolation, verifies its full
// length and CRC against the index, and only then copies its [rLo, rHi)
// fragment into dst. It returns the number of bytes written.
func (ir *IndexedReader) decodeBlockRange(b BlockInfo, dst []byte, rLo, rHi int64, st *rleStats) (int, error) {
	// Underlying reads are confined to the byte-rounded window of the block's
	// own bit range: no stream header, footer, or neighboring block byte is
	// ever fetched.
	firstByte := b.CompressedBitOffset / 8
	lastByte := (b.CompressedBitOffset + b.CompressedBitLength - 1) / 8
	win := &rangeReaderAt{r: ir.r, base: firstByte, limit: lastByte + 1}

	dec := decPool.Get().(*blockDecoder)
	defer decPool.Put(dec)
	dec.rd.Init(win)

	var derr error
	var postBWT []byte
	var storedCRC uint32
	func() {
		defer errors.Recover(&derr)
		// Skip the bits of the previous block sharing the first byte.
		if skip := uint(b.CompressedBitOffset - firstByte*8); skip > 0 {
			dec.rd.ReadBits(skip)
		}
		if dec.rd.ReadBitsBE64(48) != blkMagic {
			panicf(errors.Corrupted, "invalid block magic")
		}
		postBWT, storedCRC = dec.decodeBlock(b.Level)
	}()
	if derr != nil {
		return 0, errWrap(derr, errors.Corrupted)
	}

	// First pass: validate the entire block, including the bytes beyond the
	// requested fragment, without publishing anything.
	gotSize, gotCRC := dec.drainRLE1(postBWT, nil, 0, 0, st)
	if gotSize != b.RawSize || gotCRC != b.CRC || storedCRC != b.CRC {
		return 0, errorf(errors.Corrupted, "mismatching block checksum")
	}

	// Second pass: the block is proven intact, materialize the fragment.
	dec.drainRLE1(postBWT, dst, rLo, rHi, nil)
	return len(dst), nil
}

// rawSize returns the total decompressed size covered by the index.
func (idx *Index) rawSize() int64 {
	if len(idx.blocks) == 0 {
		return 0
	}
	last := idx.blocks[len(idx.blocks)-1]
	return last.RawOffset + last.RawSize
}

// blockSearch returns the index of the first block whose half-open raw range
// ends after off.
func blockSearch(blocks []BlockInfo, off int64) int {
	i, j := 0, len(blocks)
	for i < j {
		h := int(uint(i+j) >> 1)
		if blocks[h].RawOffset+blocks[h].RawSize <= off {
			i = h + 1
		} else {
			j = h
		}
	}
	return i
}
