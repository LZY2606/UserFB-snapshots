// Copyright 2015, Joe Tsai. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.md file.

package bzip2

import "github.com/dsnet/compress/internal/errors"

// rleDone is a special "error" to indicate that the RLE stage is done.
var rleDone = errorf(errors.Unknown, "RLE1 stage is completed")

// runLengthEncoding implements the first RLE stage of bzip2. Every sequence
// of 4..255 duplicated bytes is replaced by only the first 4 bytes, and a
// single byte representing the repeat length. Similar to the C bzip2
// implementation, the encoder will always terminate repeat sequences with a
// count (even if it is the end of the buffer), and it will also never produce
// run lengths of 256..259. The decoder can handle the latter case.
//
// For example, if the input was:
//
//	input:  "AAAAAAABBBBCCCD"
//
// Then the output will be:
//
//	output: "AAAA\x03BBBB\x00CCCD"
type runLengthEncoding struct {
	buf     []byte
	idx     int
	lastVal byte
	lastCnt int
}

func (rle *runLengthEncoding) Init(buf []byte) {
	*rle = runLengthEncoding{buf: buf}
}

func (rle *runLengthEncoding) Write(buf []byte) (int, error) {
	for i, b := range buf {
		if rle.lastVal != b {
			rle.lastCnt = 0
		}
		rle.lastCnt++
		switch {
		case rle.lastCnt < 4:
			if rle.idx >= len(rle.buf) {
				return i, rleDone
			}
			rle.buf[rle.idx] = b
			rle.idx++
		case rle.lastCnt == 4:
			if rle.idx+1 >= len(rle.buf) {
				return i, rleDone
			}
			rle.buf[rle.idx] = b
			rle.idx++
			rle.buf[rle.idx] = 0
			rle.idx++
		case rle.lastCnt < 256:
			rle.buf[rle.idx-1]++
		default:
			if rle.idx >= len(rle.buf) {
				return i, rleDone
			}
			rle.lastCnt = 1
			rle.buf[rle.idx] = b
			rle.idx++
		}
		rle.lastVal = b
	}
	return len(buf), nil
}

func (rle *runLengthEncoding) Read(buf []byte) (int, error) {
	for i := range buf {
		switch {
		case rle.lastCnt == -4:
			if rle.idx >= len(rle.buf) {
				return i, errorf(errors.Corrupted, "missing terminating run-length repeater")
			}
			rle.lastCnt = int(rle.buf[rle.idx])
			rle.idx++
			if rle.lastCnt > 0 {
				break // Break the switch
			}
			fallthrough // Count was zero, continue the work
		case rle.lastCnt <= 0:
			if rle.idx >= len(rle.buf) {
				return i, rleDone
			}
			b := rle.buf[rle.idx]
			rle.idx++
			if b != rle.lastVal {
				rle.lastCnt = 0
				rle.lastVal = b
			}
		}
		buf[i] = rle.lastVal
		rle.lastCnt--
	}
	return len(buf), nil
}

func (rle *runLengthEncoding) Bytes() []byte { return rle.buf[:rle.idx] }

// rle1Seg is one piece of the RLE1 expansion produced by rle1Stream. If lit is
// non-nil, it holds cnt literal bytes directly; otherwise the segment is a run
// of cnt copies of val. Literal segments alias the post-BWT block buffer and
// never copy it.
type rle1Seg struct {
	lit []byte
	val byte
	cnt int64
}

// rle1Stream parses the RLE1 encoding of a post-BWT block buffer without
// expanding it. The encoded grammar is literal bytes, with every four equal
// anchor bytes followed by one count byte that adds that many more copies.
// Instead of emitting one byte at a time it reports literal slices and run
// lengths. Long runs therefore stay O(1) in memory and the caller can advance
// the CRC by run-length mathematics rather than feeding plaintext bytes.
type rle1Stream struct {
	buf    []byte
	idx    int
	runVal byte // Value of a pending count-byte run
	runCnt int  // Extra copies still owed by a count byte
}

func (s *rle1Stream) next() (seg rle1Seg, ok bool) {
	// Finish a count byte seen on the previous call.
	if s.runCnt > 0 {
		k := s.runCnt
		s.runCnt = 0
		return rle1Seg{val: s.runVal, cnt: int64(k)}, true
	}
	if s.idx >= len(s.buf) {
		return rle1Seg{}, false
	}

	start := s.idx
	var last byte
	have := false
	run := 0
	for s.idx < len(s.buf) {
		b := s.buf[s.idx]
		if !have || b != last {
			last, have, run = b, true, 1
		} else {
			run++
		}
		s.idx++
		if run == 4 {
			// Four anchor bytes are followed by one repeat-count byte.
			if s.idx >= len(s.buf) {
				panicf(errors.Corrupted, "missing terminating run-length repeater")
			}
			s.runVal = last
			s.runCnt = int(s.buf[s.idx])
			s.idx++
			return rle1Seg{lit: s.buf[start : s.idx-1]}, true
		}
	}
	return rle1Seg{lit: s.buf[start:s.idx]}, true
}

// rleStats reports how the RLE1 expansion was processed. It is used by the
// package-internal tests to prove that repeated fill values travel as runs,
// not as per-plaintext-byte loops.
type rleStats struct {
	RunSegments int   // Number of run segments encountered
	RunBytes    int64 // Expanded bytes represented by runs
	LitBytes    int64 // Expanded bytes emitted by literal anchor segments
	CRCSteps    int   // Total O(log n) doubling steps in run CRC updates
	CRCUpdates  int   // Number of literal CRC updates
}

// drainRLE1 expands the post-BWT block buffer through RLE1. Every expanded
// byte is accounted for in the returned length and folded into the CRC, but
// the bytes themselves are only materialized inside dst when they fall in the
// requested half-open range [lo, hi), which is block-relative. A nil dst
// performs validation only.
func (dec *blockDecoder) drainRLE1(buf, dst []byte, lo, hi int64, st *rleStats) (n int64, crcVal uint32) {
	var stream rle1Stream
	stream.buf = buf
	var cc crc
	for {
		seg, ok := stream.next()
		if !ok {
			break
		}
		end := n + seg.cnt
		if seg.lit != nil {
			seg.cnt = int64(len(seg.lit))
			end = n + seg.cnt
			cc.update(seg.lit)
			if st != nil {
				st.CRCUpdates++
				st.LitBytes += seg.cnt
			}
			if dst != nil && end > lo && n < hi {
				a, b := max64(n, lo), min64(end, hi)
				copy(dst[a-lo:b-lo], seg.lit[a-n:b-n])
			}
		} else {
			steps := cc.updateRun(seg.val, seg.cnt)
			if st != nil {
				st.RunSegments++
				st.RunBytes += seg.cnt
				st.CRCSteps += steps
			}
			if dst != nil && end > lo && n < hi {
				a, b := max64(n, lo), min64(end, hi)
				for i := a; i < b; i++ {
					dst[i-lo] = seg.val
				}
			}
		}
		n = end
	}
	return n, cc.val
}

func max64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func min64(a, b int64) int64 {
	if a < b {
		return a
	}
	return b
}
