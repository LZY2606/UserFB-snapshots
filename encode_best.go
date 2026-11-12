// Copyright 2011 The Snappy-Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package snappy

import (
	"encoding/binary"
	"io"
)

// This file implements EncodeBest, a size-optimal Snappy block encoder. Unlike
// Encode, which is a hash-table greedy encoder optimized for speed, EncodeBest
// finds the shortest legal Snappy block representation of the input.
//
// The encoder is a shortest-path computation over the n+1 prefix lengths of
// the input. An edge j -> i represents one legal token spanning src[j:i]:
//   - a non-empty literal, costing the payload plus its tag header (1, 2 or 3
//     bytes depending on the payload length), or
//   - a COPY_1 / COPY_2 copy, costing 2 respectively 3 tag bytes.
//
// Because every edge is considered in one dynamic program, the cost of short
// versus long copies and the tag-header jumps at literal payload lengths 60/61
// and 256/257 are decided jointly; no literal boundaries are fixed in advance
// and no greedy match choice is made.
//
// Match discovery is exact: a suffix array (linear-time SA-IS) and the Kasai
// LCP array identify every repeated prefix. A table of the nearest earlier
// occurrence of every 1..64 byte prefix is derived in O(64n) time. No n*n
// match table is allocated and no prefix is scanned from every position.

// bestMaxCopyLen is the longest copy that a single Snappy tag can emit
// (COPY_2 carries length 1+m, m in [0,64)).
const bestMaxCopyLen = 64

// literalTagLen returns the tag-header size, excluding payload bytes, of a
// literal token carrying n payload bytes. The wire rules are:
//
//	1 <= n <= 60:   one tag byte (m = n-1 < 60)
//	61 <= n <= 256: tag byte plus one length byte (m = 60)
//	257 <= n:       tag byte plus two length bytes (m = 61)
func literalTagLen(n int) int {
	if n <= 60 {
		return 1
	}
	if n <= 256 {
		return 2
	}
	return 3
}

// EncodeBest returns the shortest encoded form of src. The returned slice may
// be a sub-slice of dst if dst was large enough to hold the entire encoded
// block. Otherwise, a newly allocated slice will be returned.
//
// The dst and src must not overlap. It is valid to pass a nil dst. Bytes of
// dst outside the returned slice are not written.
//
// EncodeBest handles the Snappy block format, not the Snappy stream format.
// The input must be at most 65536 bytes, the maximum single-block size;
// otherwise ErrTooLarge is returned and dst is not modified.
//
// The output is a global minimum-length Snappy block for src over the token
// set {non-empty literal, COPY_1, COPY_2}: every emitted token is decodable by
// the existing Decode, and copies only reference the already emitted prefix.
// EncodeBest is deterministic: identical inputs produce identical bytes on
// every architecture and build configuration.
func EncodeBest(dst, src []byte) ([]byte, error) {
	if len(src) > maxBlockSize {
		return nil, ErrTooLarge
	}
	if n := MaxEncodedLen(len(src)); len(dst) < n {
		dst = make([]byte, n)
	}

	// The block starts with the varint-encoded length of the decompressed bytes.
	h := binary.PutUvarint(dst, uint64(len(src)))
	if len(src) == 0 {
		return dst[:h], nil
	}

	d := encodeBestBlock(dst[h:], src)
	return dst[:h+d], nil
}

// encodeBestBlock writes the token stream of src into dst (which is guaranteed
// large enough) and returns the number of bytes written.
//
// Worst-case work, for n = len(src) <= 65536:
//   - Match discovery. bestSAIS is SA-IS with an explicit sentinel: every
//     recursion level is linear and the reduced string is at most half the
//     size, so the levels form a geometric series and the total is O(n).
//     bestKasai is the linear LCP construction, O(n). The nearest-occurrence
//     table is 64 group-assignment/sweep passes, each an O(n) walk, i.e.
//     O(64n). No position ever scans the prefix it sits in and no n*n match
//     table is built.
//   - Path selection. The DP performs one pass over the n+1 prefix lengths.
//     At each prefix it examines at most 60 class-1 literal starts, at most
//     196 class-2 starts, one running-minimum class-3 update and at most 64
//     copy lengths: a fixed amount of work per prefix, hence O(n) overall.
//     Literal candidates are never enumerated per remaining length; the long
//     literal class is handled by the running minimum. Reconstruction and
//     emission are O(n).
//
// The total worst-case time is therefore O(n) with small explicit constants
// (64 match passes plus <= 321 DP relaxations per prefix), with no quadratic
// term. Extra memory is O(n): the dominant allocation is the 64*n uint16
// nearest-offset table (128n bytes); the suffix-array, LCP/rank, DP and
// reconstruction arrays add a constant number of n-sized vectors, and the
// SA-IS recursion halves its working set per level, so its stacked
// allocations sum to at most a constant multiple of n.
func encodeBestBlock(dst, src []byte) int {
	n := len(src)

	// Exact match discovery: suffix array, LCP and rank arrays.
	x := make([]int32, n)
	for i, c := range src {
		x[i] = int32(c)
	}
	sa := make([]int32, n)
	bestSAIS(x, sa, 256)
	lcp := make([]int32, n)
	rank := make([]int32, n)
	bestKasai(x, sa, rank, lcp)

	// offs[s*bestMaxCopyLen+(l-1)] is the distance to the nearest earlier
	// occurrence of the l-byte prefix of suffix s, or 0 if none exists. The
	// nearest occurrence is the only one the DP needs: COPY_2 costs 3 bytes at
	// any offset, and COPY_1 is available exactly when the nearest offset is at
	// most 2047. It is built one copy length at a time. The suffix array makes
	// the suffixes sharing an l-byte prefix a contiguous rank range (a group);
	// a text-order sweep with one "latest position" slot per group then yields
	// the answer in O(n) per length, hence O(64n) overall.
	offs := make([]uint16, n*bestMaxCopyLen)
	groupID := make([]int32, n)
	latest := make([]int32, n)
	for l := 1; l <= bestMaxCopyLen; l++ {
		// Assign group ids by walking suffix-array ranks: a new group starts
		// whenever the adjacent LCP is shorter than l.
		g := int32(0)
		groupID[0] = 0
		for r := 1; r < n; r++ {
			if lcp[r] < int32(l) {
				g++
			}
			groupID[r] = g
		}
		for i := range latest[:g+1] {
			latest[i] = -1
		}
		for s := 0; s+l <= n; s++ {
			id := groupID[rank[s]]
			p := latest[id]
			if p >= 0 {
				offs[s*bestMaxCopyLen+l-1] = uint16(s - int(p))
			}
			latest[id] = int32(s)
		}
	}

	// Shortest-path DP over prefix lengths. cost[i] is the minimum token-stream
	// length (excluding the block header, which is fixed for a given n) that
	// emits src[:i].
	const inf = int32(1 << 30)
	cost := make([]int32, n+1)
	for i := 1; i <= n; i++ {
		cost[i] = inf
	}
	// Reconstruction records: token kind at prefix i (1 literal, 2 COPY_1,
	// 3 COPY_2), its length and, for copies, its offset. The length uses 32
	// bits because a literal can span the full 65536-byte block, which does
	// not fit in a uint16.
	dpOp := make([]uint8, n+1)
	dpLen := make([]uint32, n+1)
	dpOff := make([]uint16, n+1)

	// For literals of tag class 3 (payload >= 257 bytes), every boundary
	// h <= i-257 is eligible regardless of how old it is. As i grows only one
	// new boundary enters the range, so a running minimum suffices. An edge from
	// h costs cost[h] + (i-h) + 3 = (i+3) + (cost[h]-h), hence the running
	// value minimizes cost[h]-h.
	var min3Val int32 = inf
	var min3H int

	for i := 1; i <= n; i++ {
		best := inf
		bestOp := uint8(0)
		var bestLen uint32
		var bestOff uint16

		// Literal edges. Every prefix position h is a legal literal start:
		// literals need not align with copy positions, so no boundary set is
		// used. The windows below are bounded by payload length, hence no
		// position enumerates all remaining lengths.
		//
		// Class 1: payload 1..60, starts h in [i-60, i-1].
		lo := i - 60
		if lo < 0 {
			lo = 0
		}
		for h := lo; h < i; h++ {
			lit := i - h
			c := cost[h] + int32(lit+1)
			if c < best {
				best, bestOp = c, 1
				bestLen, bestOff = uint32(lit), 0
			}
		}
		// Class 2: payload 61..256, starts h in [i-256, i-61].
		lo = i - 256
		if lo < 0 {
			lo = 0
		}
		hi := i - 61
		for h := lo; h <= hi; h++ {
			lit := i - h
			c := cost[h] + int32(lit+2)
			if c < best {
				best, bestOp = c, 1
				bestLen, bestOff = uint32(lit), 0
			}
		}
		// Class 3: payload >= 257, starts h <= i-257.
		if h := i - 257; h >= 0 {
			if v := cost[h] - int32(h); v < min3Val {
				min3Val, min3H = v, h
			}
		}
		if min3Val < inf {
			if c := min3Val + int32(i+3); c < best {
				best, bestOp = c, 1
				bestLen, bestOff = uint32(i-min3H), 0
			}
		}

		// Copy edges. A length-l copy ending at i starts at s = i-l and is
		// legal exactly when the l-prefix of suffix s occurred earlier.
		maxL := bestMaxCopyLen
		if maxL > i {
			maxL = i
		}
		for l := 1; l <= maxL; l++ {
			s := i - l
			o := offs[s*bestMaxCopyLen+l-1]
			if o == 0 {
				continue
			}
			tag := 3 // COPY_2: 3 bytes, lengths 1..64, offsets 1..65535
			// COPY_1 costs 2 bytes but is restricted to lengths 4..11 and
			// offsets 1..2047.
			if l >= 4 && l <= 11 && o <= 2047 {
				tag = 2
			}
			if c := cost[i-l] + int32(tag); c < best {
				best, bestOp = c, uint8(tag)
				bestLen, bestOff = uint32(l), o
			}
		}

		cost[i], dpOp[i], dpLen[i], dpOff[i] = best, bestOp, bestLen, bestOff
	}

	// Reconstruct the chosen parse by walking prefix lengths backwards, then
	// emit the tokens forwards.
	ends := make([]int, 0, n)
	for i := n; i > 0; {
		ends = append(ends, i)
		i -= int(dpLen[i])
	}
	d := 0
	for k := len(ends) - 1; k >= 0; k-- {
		i := ends[k]
		l := int(dpLen[i])
		start := i - l
		switch dpOp[i] {
		case 1:
			d += emitLiteral(dst[d:], src[start:i])
		case 2: // COPY_1
			o := int(dpOff[i])
			dst[d+0] = uint8(o>>8)<<5 | uint8(l-4)<<2 | tagCopy1
			dst[d+1] = uint8(o)
			d += 2
		default: // COPY_2
			o := int(dpOff[i])
			dst[d+0] = uint8(l-1)<<2 | tagCopy2
			dst[d+1] = uint8(o)
			dst[d+2] = uint8(o >> 8)
			d += 3
		}
	}
	return d
}

// bestSAIS computes the suffix array of x into sa using the linear-time
// induced-sorting (SA-IS) algorithm. Symbols of x are bytes in [0,256).
// A unique smallest sentinel is appended for the construction; the returned
// suffix array covers the n original positions only.
func bestSAIS(x, sa []int32, k int) {
	n := len(x)
	// Build the working string with an explicit unique smallest sentinel.
	// Real symbols are shifted up by one so the sentinel stays unique.
	s := make([]int32, n+1)
	for i, c := range x {
		s[i] = c + 1
	}
	full := make([]int32, n+1)
	bestSAISCore(s, full, k+1)
	for i := 0; i < n; i++ {
		sa[i] = full[i+1] // full[0] is the suffix consisting of the sentinel
	}
}

// bestSAISCore is the recursive SA-IS kernel. The precondition is that
// s[m-1] is a unique smallest sentinel symbol (0) and all other symbols are in
// [1, k); on return sa holds the suffix array of s.
func bestSAISCore(s, sa []int32, k int) {
	m := len(s)
	if m == 1 {
		sa[0] = 0
		return
	}
	if m == 2 {
		if s[0] < s[1] {
			sa[0], sa[1] = 0, 1
		} else {
			sa[0], sa[1] = 1, 0
		}
		return
	}

	// Classify suffixes: S-type (true) or L-type (false). The sentinel suffix
	// is S-type; its predecessor is necessarily L-type.
	t := make([]bool, m)
	t[m-1] = true
	for i := m - 2; i >= 0; i-- {
		t[i] = s[i] < s[i+1] || (s[i] == s[i+1] && t[i+1])
	}
	isLMS := func(i int) bool { return i > 0 && t[i] && !t[i-1] }

	bucketSize := make([]int32, k)
	for _, c := range s {
		bucketSize[c]++
	}
	bucketEnds := func() []int32 {
		b := make([]int32, k)
		var sum int32
		for c := 0; c < k; c++ {
			sum += bucketSize[c]
			b[c] = sum
		}
		return b
	}
	bucketStarts := func() []int32 {
		b := make([]int32, k)
		var sum int32
		for c := 0; c < k; c++ {
			b[c] = sum
			sum += bucketSize[c]
		}
		return b
	}
	induceL := func() {
		b := bucketStarts()
		for i := 0; i < m; i++ {
			j := int(sa[i]) - 1
			if j >= 0 && !t[j] {
				c := s[j]
				sa[b[c]] = int32(j)
				b[c]++
			}
		}
	}
	induceS := func() {
		b := bucketEnds()
		for i := m - 1; i >= 0; i-- {
			j := int(sa[i]) - 1
			if j >= 0 && t[j] {
				c := s[j]
				b[c]--
				sa[b[c]] = int32(j)
			}
		}
	}

	// Stage 1: approximate sort of the LMS suffixes.
	for i := range sa {
		sa[i] = -1
	}
	b := bucketEnds()
	for i := m - 1; i >= 1; i-- {
		if isLMS(i) {
			c := s[i]
			b[c]--
			sa[b[c]] = int32(i)
		}
	}
	induceL()
	induceS()

	// Compact the sorted LMS positions.
	lmsSorted := make([]int32, 0, m/2)
	for i := 0; i < m; i++ {
		if p := int(sa[i]); isLMS(p) {
			lmsSorted = append(lmsSorted, int32(p))
		}
	}
	n1 := len(lmsSorted)

	// Name the LMS substrings; equal substrings get equal names. The sentinel
	// LMS substring sorts first and therefore receives name 0.
	for i := range sa {
		sa[i] = -1
	}
	var name int32
	prev := -1
	for _, q := range lmsSorted {
		pos := int(q)
		if prev >= 0 {
			for d := 0; pos+d < m && prev+d < m; d++ {
				if s[pos+d] != s[prev+d] || t[pos+d] != t[prev+d] {
					name++
					break
				}
				if d > 0 && (isLMS(pos+d) || isLMS(prev+d)) {
					break
				}
			}
		}
		// LMS positions are at least two apart, so pos/2 slots never collide.
		sa[pos/2] = name
		prev = pos
	}
	// Compact the names to the end of sa; the reduced string lives there.
	j := m - 1
	for i := m - 1; i >= 0; i-- {
		if sa[i] >= 0 {
			sa[j] = sa[i]
			j--
		}
	}
	s1 := sa[m-n1:]
	if int(name)+1 < n1 {
		// Names are not unique: recurse on the reduced problem. The reduced
		// string already ends with the unique smallest name (the sentinel LMS
		// substring), so it satisfies the kernel precondition.
		bestSAISCore(s1, sa[:n1], int(name)+1)
	} else {
		for i := 0; i < n1; i++ {
			sa[s1[i]] = int32(i)
		}
	}

	// Stage 2: exact sort of the LMS suffixes.
	q := 0
	for i := 1; i < m; i++ {
		if isLMS(i) {
			s1[q] = int32(i)
			q++
		}
	}
	for i := 0; i < n1; i++ {
		sa[i] = s1[sa[i]]
	}
	for i := n1; i < m; i++ {
		sa[i] = -1
	}
	b = bucketEnds()
	for i := n1 - 1; i >= 0; i-- {
		p := int(sa[i])
		sa[i] = -1
		c := s[p]
		b[c]--
		sa[b[c]] = int32(p)
	}
	induceL()
	induceS()
}

// bestKasai computes the LCP array from the suffix array: lcp[r] is the
// longest common prefix of sa[r-1] and sa[r] for r >= 1 (lcp[0] = 0). rank
// receives the inverse suffix array.
func bestKasai(x, sa, rank, lcp []int32) {
	n := len(x)
	for r := 0; r < n; r++ {
		rank[sa[r]] = int32(r)
	}
	h := 0
	for i := 0; i < n; i++ {
		r := int(rank[i])
		if r == 0 {
			lcp[0] = 0
			if h > 0 {
				h--
			}
			continue
		}
		j := int(sa[r-1])
		for i+h < n && j+h < n && x[i+h] == x[j+h] {
			h++
		}
		lcp[r] = int32(h)
		if h > 0 {
			h--
		}
	}
}

// NewBestWriter returns a new buffered Writer that compresses to w using the
// framing format described at
// https://github.com/google/snappy/blob/master/framing_format.txt
//
// It offers the same buffering interface as NewBufferedWriter, but each
// at-most-65536-byte chunk is encoded with EncodeBest instead of the speed
// oriented Encode. The 12.5% compression threshold and the per-chunk CRC are
// unchanged, so the stream is read by NewReader without any private chunk
// types. The Writer returned buffers writes; call Close when done.
func NewBestWriter(w io.Writer) *Writer {
	return &Writer{
		w:    w,
		best: true,
		ibuf: make([]byte, 0, maxBlockSize),
		obuf: make([]byte, obufLen),
	}
}
