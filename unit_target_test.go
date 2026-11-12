// Copyright 2011 The Snappy-Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package snappy

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"math/rand"
	"testing"
	"time"
)

// exhaustiveOptimalLen returns the minimum token-stream length of src by
// enumerating every legal Snappy token path with dynamic programming. It is
// deliberately independent of the EncodeBest implementation and is only used
// on small inputs.
//
// Two kinds of partial parses of the prefix src[:i] are tracked:
//   - flush[i]: minimum cost of a complete token stream covering src[:i],
//   - pend[h]: minimum cost of a stream covering src[:i] where the bytes
//     src[h:i] form a not-yet-flushed literal; its tag header is not paid yet.
//
// The transitions enumerate every legal continuation:
//   - extend the pending literal by src[i] (zero added tag cost),
//   - flush it as a literal, paying the real tag header for its length,
//   - emit a length-l copy at offset o, including overlapping ones (o may be
//     smaller than l), paying 2 (COPY_1) or 3 (COPY_2) bytes.
//
// Every literal boundary and every legal copy is considered, so flush[n] is
// the true global minimum; the block header varint is added by the caller.
func exhaustiveOptimalLen(src []byte) int {
	n := len(src)
	if n == 0 {
		return 0
	}
	const inf = 1 << 30
	flush := make([]int, n+1)
	pend := make([]int, n+1)
	for i := range pend {
		pend[i] = inf
	}
	for i := 1; i <= n; i++ {
		// A pending literal ending here either starts at i-1 on top of a
		// complete stream, or extends the one carried over from row i-1.
		pend[i-1] = flush[i-1]
		best := inf
		// Flush the pending literal at every possible start position; the
		// payload bytes themselves are paid here, together with the tag
		// header whose size jumps at payload lengths 60/61 and 256/257.
		for h := 0; h < i; h++ {
			if c := pend[h] + (i - h) + literalTagLen(i-h); c < best {
				best = c
			}
		}
		// Every copy ending at i: it covers src[i-l:i] and reproduces those
		// bytes from the already emitted prefix, overlapping copies included.
		for l := 1; l <= 64 && l <= i; l++ {
			s := i - l
			for o := 1; o <= s; o++ {
				ok := true
				for k := 0; k < l; k++ {
					if src[s+k] != src[s-o+k] {
						ok = false
						break
					}
				}
				if !ok {
					continue
				}
				tagCost := 3 // COPY_2
				if l >= 4 && l <= 11 && o <= 2047 {
					tagCost = 2 // COPY_1
				}
				if c := flush[s] + tagCost; c < best {
					best = c
				}
			}
		}
		flush[i] = best
	}
	return flush[n]
}

// parseBestToken describes one decoded Snappy token.
type parsedToken struct {
	kind   uint8 // tagLiteral, tagCopy1 or tagCopy2
	start  int   // input position where the token starts
	length int   // literal payload or copy length
	offset int   // copy offset; 0 for literals
	cost   int   // number of encoded bytes the token occupies
}

// parseBestTokens parses an EncodeBest block (after its length header) into
// tokens, validating every field against the snappy.go rules and confirming
// that each copy reproduces the bytes of src.
func parseBestTokens(t *testing.T, body, src []byte) []parsedToken {
	t.Helper()
	pos := 0
	out := 0
	var toks []parsedToken
	for pos < len(body) {
		tag := body[pos]
		l := tag & 0x03
		m := tag >> 2
		switch l {
		case tagLiteral:
			var litLen int
			tagCost := 1
			switch {
			case m < 60:
				litLen = int(m) + 1
			case m == 60:
				if pos+1 >= len(body) {
					t.Fatalf("truncated literal length at %d", pos)
				}
				litLen = int(body[pos+1]) + 1
				tagCost = 2
			case m == 61:
				if pos+2 >= len(body) {
					t.Fatalf("truncated literal length at %d", pos)
				}
				litLen = int(body[pos+1]) | int(body[pos+2])<<8
				litLen++
				tagCost = 3
			default:
				t.Fatalf("unsupported literal m=%d", m)
			}
			if litLen == 0 {
				t.Fatalf("empty literal token at %d", pos)
			}
			if pos+tagCost+litLen > len(body) {
				t.Fatalf("literal overruns block at %d", pos)
			}
			if !bytes.Equal(body[pos+tagCost:pos+tagCost+litLen], src[out:out+litLen]) {
				t.Fatalf("literal payload mismatch at %d", pos)
			}
			toks = append(toks, parsedToken{tagLiteral, out, litLen, 0, tagCost + litLen})
			pos += tagCost + litLen
			out += litLen
		case tagCopy1:
			if pos+1 >= len(body) {
				t.Fatalf("truncated COPY_1 at %d", pos)
			}
			length := 4 + int(m&0x07)
			offset := int(m&0x38)<<5 | int(body[pos+1])
			if offset < 1 || offset > 2047 || length < 4 || length > 11 {
				t.Fatalf("illegal COPY_1 length=%d offset=%d", length, offset)
			}
			if offset > out {
				t.Fatalf("COPY_1 references unseen prefix: offset=%d out=%d", offset, out)
			}
			for k := 0; k < length; k++ {
				if src[out+k] != src[out-offset+k] {
					t.Fatalf("COPY_1 produces wrong byte at %d", out+k)
				}
			}
			toks = append(toks, parsedToken{tagCopy1, out, length, offset, 2})
			pos += 2
			out += length
		case tagCopy2:
			if pos+2 >= len(body) {
				t.Fatalf("truncated COPY_2 at %d", pos)
			}
			length := 1 + int(m)
			offset := int(body[pos+1]) | int(body[pos+2])<<8
			if length < 1 || length > 64 || offset < 1 || offset > 65535 {
				t.Fatalf("illegal COPY_2 length=%d offset=%d", length, offset)
			}
			if offset > out {
				t.Fatalf("COPY_2 references unseen prefix: offset=%d out=%d", offset, out)
			}
			for k := 0; k < length; k++ {
				if src[out+k] != src[out-offset+k] {
					t.Fatalf("COPY_2 produces wrong byte at %d", out+k)
				}
			}
			toks = append(toks, parsedToken{tagCopy2, out, length, offset, 3})
			pos += 3
			out += length
		default:
			t.Fatalf("COPY_4 is not an allowed output token, tag=%#x", tag)
		}
	}
	if out != len(src) {
		t.Fatalf("tokens cover %d bytes, want %d", out, len(src))
	}
	return toks
}

// checkEncodeBest runs EncodeBest and verifies: the header, the exact
// optimality of the total length against the exhaustive reference (when small
// enough), per-token legality and byte cost, the Decode round trip, and that
// bytes outside the returned slice of a reused dst are untouched.
func checkEncodeBest(t *testing.T, src []byte) []byte {
	t.Helper()

	// Header length reference.
	var hdr [binary.MaxVarintLen64]byte
	headerLen := binary.PutUvarint(hdr[:], uint64(len(src)))

	enc, err := EncodeBest(nil, src)
	if err != nil {
		t.Fatalf("EncodeBest: %v", err)
	}
	if len(enc) < headerLen {
		t.Fatalf("output shorter than header: %d", len(enc))
	}
	dLen, hLen, err := decodedLen(enc)
	if err != nil || dLen != len(src) || hLen != headerLen {
		t.Fatalf("bad block header: dLen=%d hLen=%d err=%v", dLen, hLen, err)
	}
	toks := parseBestTokens(t, enc[headerLen:], src)
	tokenCost := 0
	for _, tk := range toks {
		tokenCost += tk.cost
	}
	if headerLen+tokenCost != len(enc) {
		t.Fatalf("declared length %d != header %d + sum of token costs %d", len(enc), headerLen, tokenCost)
	}

	if len(src) <= 24 {
		want := headerLen + exhaustiveOptimalLen(src)
		if len(enc) != want {
			t.Fatalf("not optimal: got %d bytes, exhaustive minimum is %d; enc=% x", len(enc), want, enc)
		}
	}

	dec, err := Decode(nil, enc)
	if err != nil || !bytes.Equal(dec, src) {
		t.Fatalf("Decode round trip failed: err=%v", err)
	}

	// Reuse a pre-filled dst; bytes beyond the returned slice must survive.
	// The semantics are size-independent, so this runs only on small inputs to
	// avoid doubling the work of every large-material check.
	if len(src) <= 256 {
		dst := make([]byte, MaxEncodedLen(len(src))+7)
		for i := range dst {
			dst[i] = 0xa5
		}
		out, err := EncodeBest(dst, src)
		if err != nil {
			t.Fatalf("EncodeBest with reused dst: %v", err)
		}
		if !bytes.Equal(out, enc) {
			t.Fatalf("reused-dst output differs from nil-dst output")
		}
		for i := len(out); i < len(dst); i++ {
			if dst[i] != 0xa5 {
				t.Fatalf("byte %d outside returned slice was modified", i)
			}
		}
	}
	return enc
}

func TestUnitOptimal(t *testing.T) {
	t.Run("TenAesShortCopyWitness", func(t *testing.T) {
		src := bytes.Repeat([]byte("a"), 10)
		enc := checkEncodeBest(t, src)
		greedy := Encode(nil, src)
		if len(enc) >= len(greedy) {
			t.Fatalf("EncodeBest did not beat greedy: best=%d greedy=%d", len(enc), len(greedy))
		}
		if len(enc) != 5 {
			t.Fatalf("ten 'a' should take 5 bytes (header+literal+COPY_1), got %d: % x", len(enc), enc)
		}
	})

	t.Run("ExhaustiveAllSmall", func(t *testing.T) {
		// Every binary string up to length 10: brute force over the full
		// alphabet gives complete coverage of small-token interactions.
		for n := 0; n <= 10; n++ {
			buf := make([]byte, n)
			var rec func(int)
			count := 0
			rec = func(k int) {
				if k == n {
					checkEncodeBest(t, buf)
					count++
					return
				}
				for _, c := range []byte{0, 1} {
					buf[k] = c
					rec(k + 1)
				}
			}
			rec(0)
			if count != 1<<uint(n) {
				t.Fatalf("enumeration count mismatch at n=%d", n)
			}
		}
	})

	t.Run("ExhaustiveRandomLarger", func(t *testing.T) {
		rng := rand.New(rand.NewSource(1))
		// Randomized inputs over small alphabets up to length 24, where the
		// exhaustive reference still runs.
		for trial := 0; trial < 400; trial++ {
			n := 1 + rng.Intn(24)
			alpha := 1 + rng.Intn(4)
			src := make([]byte, n)
			for i := range src {
				src[i] = byte(rng.Intn(alpha))
			}
			checkEncodeBest(t, src)
		}
	})

	t.Run("SuffixArrayAndLCPExtract", func(t *testing.T) {
		// The match-discovery core must be exact on its own: brute-force
		// suffix ordering and LCPs are compared against bestSAIS/bestKasai on
		// exhaustive small strings and adversarial periodic/random ones.
		rng := rand.New(rand.NewSource(7))
		check := func(src []byte) {
			t.Helper()
			n := len(src)
			x := make([]int32, n)
			for i, c := range src {
				x[i] = int32(c)
			}
			sa := make([]int32, n)
			bestSAIS(x, sa, 256)
			seen := make([]bool, n)
			for _, p := range sa {
				if p < 0 || int(p) >= n || seen[p] {
					t.Fatalf("bad suffix array for % x: %v", src, sa)
				}
				seen[p] = true
			}
			for r := 1; r < n; r++ {
				if bytes.Compare(src[sa[r-1]:], src[sa[r]:]) >= 0 {
					t.Fatalf("suffix array not sorted at %d for % x", r, src)
				}
			}
			lcp := make([]int32, n)
			rank := make([]int32, n)
			bestKasai(x, sa, rank, lcp)
			for i := range rank {
				if sa[rank[i]] != int32(i) {
					t.Fatalf("rank is not the inverse suffix array for % x", src)
				}
			}
			for r := 1; r < n; r++ {
				a, b := src[sa[r-1]:], src[sa[r]:]
				want := 0
				for want < len(a) && want < len(b) && a[want] == b[want] {
					want++
				}
				if int(lcp[r]) != want {
					t.Fatalf("lcp[%d]=%d want %d for % x", r, lcp[r], want, src)
				}
			}
		}
		for n := 0; n <= 12; n++ {
			buf := make([]byte, n)
			var rec func(int)
			rec = func(k int) {
				if k == n {
					check(buf)
					return
				}
				for _, c := range []byte{0, 1} {
					buf[k] = c
					rec(k + 1)
				}
			}
			rec(0)
		}
		for trial := 0; trial < 2000; trial++ {
			n := 1 + rng.Intn(200)
			src := make([]byte, n)
			for i := range src {
				src[i] = byte(rng.Intn(1 + rng.Intn(256)))
			}
			check(src)
		}
		for _, p := range []int{1, 2, 3, 5, 7, 13, 64} {
			for _, n := range []int{p, p + 1, 2*p + 1, 1000, 4097} {
				src := make([]byte, n)
				for i := range src {
					src[i] = byte(i % p)
				}
				check(src)
			}
		}
		for trial := 0; trial < 10; trial++ {
			src := make([]byte, 1000+rng.Intn(64537))
			rng.Read(src)
			check(src)
		}
	})

	t.Run("ShortAndLongCopyCosts", func(t *testing.T) {
		// A run long enough that COPY_1 (length 4..11) and COPY_2 chaining
		// compete; the exhaustive reference decides which is shorter.
		for _, n := range []int{12, 13, 20, 30, 63, 64, 65, 100} {
			checkEncodeBest(t, bytes.Repeat([]byte("z"), n))
		}
		// Overlapping distance-1 copies must be used on runs.
		run := bytes.Repeat([]byte("q"), 200)
		enc := checkEncodeBest(t, run)
		_, hLen, err := decodedLen(enc)
		if err != nil {
			t.Fatal(err)
		}
		toks := parseBestTokens(t, enc[hLen:], run)
		sawDistanceOne := false
		for _, tk := range toks {
			if tk.kind != tagLiteral && tk.offset == 1 {
				sawDistanceOne = true
			}
		}
		if !sawDistanceOne {
			t.Fatalf("expected an overlapping distance-1 copy on a run")
		}
	})
}

func TestUnitBoundaries(t *testing.T) {
	t.Run("Empty", func(t *testing.T) {
		enc := checkEncodeBest(t, nil)
		if len(enc) != 1 || enc[0] != 0 {
			t.Fatalf("empty input must be a single zero byte, got % x", enc)
		}
	})

	t.Run("TooLarge", func(t *testing.T) {
		big := make([]byte, maxBlockSize+1)
		dst := make([]byte, 16)
		for i := range dst {
			dst[i] = 0x5a
		}
		out, err := EncodeBest(dst, big)
		if !errors.Is(err, ErrTooLarge) {
			t.Fatalf("want ErrTooLarge, got %v", err)
		}
		if out != nil {
			t.Fatalf("want nil output on error, got %d bytes", len(out))
		}
		for i, c := range dst {
			if c != 0x5a {
				t.Fatalf("dst was modified at %d after ErrTooLarge", i)
			}
		}
		// Exactly the limit is accepted.
		atLimit := make([]byte, maxBlockSize)
		if _, err := EncodeBest(nil, atLimit); err != nil {
			t.Fatalf("input of %d bytes must be accepted: %v", maxBlockSize, err)
		}
	})

	t.Run("LiteralHeaderJumpLengths", func(t *testing.T) {
		// Incompressible inputs force pure-literal parses; exercise the tag
		// header jumps at 60/61 and 256/257 and the nearby lengths.
		rng := rand.New(rand.NewSource(2))
		for _, n := range []int{59, 60, 61, 62, 255, 256, 257, 258, 1000} {
			src := make([]byte, n)
			rng.Read(src)
			enc := checkEncodeBest(t, src)
			_, hLen, err := decodedLen(enc)
			if err != nil {
				t.Fatal(err)
			}
			toks := parseBestTokens(t, enc[hLen:], src)
			// A single literal is always a legal parse costing n + header(n),
			// so the optimum can never exceed it. Every token must also charge
			// exactly the bytes it occupies.
			total := 0
			for _, tk := range toks {
				if tk.kind == tagLiteral && tk.cost != tk.length+literalTagLen(tk.length) {
					t.Fatalf("n=%d: literal header cost wrong: %+v", n, tk)
				}
				total += tk.cost
			}
			if total > n+literalTagLen(n) {
				t.Fatalf("n=%d: token cost %d exceeds pure-literal parse %d", n, total, n+literalTagLen(n))
			}
		}
	})

	t.Run("CopyRangeBoundaries", func(t *testing.T) {
		// A far COPY_2: random base (so the planted 64 bytes have no nearer
		// occurrence) with one 64-byte block duplicated near the end. One
		// length-64 copy covers the block in 3 bytes; any split needs at least
		// two tokens, so the optimum must contain the far max-length copy.
		src := make([]byte, 65536)
		rand.New(rand.NewSource(8)).Read(src)
		copy(src[65536-64:], src[8:8+64])
		enc := checkEncodeBest(t, src)
		_, hLen, err := decodedLen(enc)
		if err != nil {
			t.Fatal(err)
		}
		toks := parseBestTokens(t, enc[hLen:], src)
		sawFar := false
		for _, tk := range toks {
			if tk.kind == tagCopy2 && tk.offset == 65536-64-8 && tk.length == 64 {
				sawFar = true
			}
			if tk.kind == tagCopy1 && (tk.length < 4 || tk.length > 11 || tk.offset > 2047) {
				t.Fatalf("COPY_1 out of range: %+v", tk)
			}
			if tk.kind == tagCopy2 && (tk.length > 64 || tk.offset > 65535) {
				t.Fatalf("COPY_2 out of range: %+v", tk)
			}
		}
		if !sawFar {
			t.Fatalf("expected a far max-length COPY_2 at offset %d", 65536-64-8)
		}
		// Offset 2047 vs 2048 decides COPY_1 (2 bytes) versus COPY_2 (3 bytes)
		// for an otherwise identical marker. The marker's four 7s are followed
		// by zeros, so the match legitimately extends past four bytes; what is
		// pinned is its start, offset, tag kind and real byte cost.
		for _, c := range []struct {
			off      int
			wantKind uint8
			wantCost int
		}{{2047, tagCopy1, 2}, {2048, tagCopy2, 3}} {
			s := make([]byte, c.off+8)
			s[0], s[1], s[2], s[3] = 7, 7, 7, 7
			s[c.off], s[c.off+1], s[c.off+2], s[c.off+3] = 7, 7, 7, 7
			enc := checkEncodeBest(t, s)
			_, hLen, _ := decodedLen(enc)
			toks := parseBestTokens(t, enc[hLen:], s)
			found := false
			for _, tk := range toks {
				if tk.start == c.off && tk.offset == c.off && tk.kind == c.wantKind {
					found = true
					if tk.cost != c.wantCost {
						t.Fatalf("offset %d: want %d tag bytes, got %d", c.off, c.wantCost, tk.cost)
					}
				}
			}
			if !found {
				t.Fatalf("offset %d: expected a %d-byte copy starting at the marker", c.off, c.wantCost)
			}
		}
	})

	t.Run("DeterminismAcrossConfigurations", func(t *testing.T) {
		// Golden bytes pin the exact representation; the noasm build runs the
		// same pure-Go code and must reproduce these identical bytes, so the
		// native and noasm configurations cannot drift apart.
		golden := []struct {
			src string
			enc []byte
		}{
			{"", []byte{0x00}},
			{"a", []byte{0x01, 0x00, 0x61}},
			{"aaaaaaaaaa", []byte{0x0a, 0x00, 0x61, 0x15, 0x01}},
			{"abcabcabcabc", []byte{0x0c, 0x08, 0x61, 0x62, 0x63, 0x15, 0x03}},
			{"\x00\x00\x00\x00\x00\x01", []byte{0x06, 0x00, 0x00, 0x01, 0x01, 0x00, 0x01}},
			{"the quick brown fox jumps over the lazy dog; the quick brown fox",
				[]byte{0x40, 0x78, 0x74, 0x68, 0x65, 0x20, 0x71, 0x75, 0x69, 0x63, 0x6b, 0x20,
					0x62, 0x72, 0x6f, 0x77, 0x6e, 0x20, 0x66, 0x6f, 0x78, 0x20, 0x6a, 0x75,
					0x6d, 0x70, 0x73, 0x20, 0x6f, 0x76, 0x65, 0x72, 0x20, 0x01, 0x1f, 0x24,
					0x6c, 0x61, 0x7a, 0x79, 0x20, 0x64, 0x6f, 0x67, 0x3b, 0x20, 0x4a, 0x2d,
					0x00}},
		}
		for _, g := range golden {
			got, err := EncodeBest(nil, []byte(g.src))
			if err != nil {
				t.Fatal(err)
			}
			if !bytes.Equal(got, g.enc) {
				t.Fatalf("golden mismatch for %q:\n got % x\nwant % x", g.src, got, g.enc)
			}
		}

		// Determinism is structural (strict-comparison DP over exact matches),
		// but also pin repeated outputs byte-for-byte on random inputs.
		rng := rand.New(rand.NewSource(3))
		for trial := 0; trial < 20; trial++ {
			n := 1 + rng.Intn(300)
			src := make([]byte, n)
			for i := range src {
				src[i] = byte(rng.Intn(6))
			}
			first, err := EncodeBest(nil, src)
			if err != nil {
				t.Fatal(err)
			}
			for rep := 0; rep < 3; rep++ {
				again, err := EncodeBest(nil, src)
				if err != nil {
					t.Fatal(err)
				}
				if !bytes.Equal(first, again) {
					t.Fatalf("non-deterministic output on trial %d rep %d", trial, rep)
				}
			}
		}
	})
}

// largeMaterials returns the four required demonstration materials.
func largeMaterials() map[string][]byte {
	out := map[string][]byte{}

	// 64 KiB all identical.
	out["all-same"] = bytes.Repeat([]byte("a"), maxBlockSize)

	// Short period: a 7-byte cycle repeated.
	period := []byte("snappy!")
	short := make([]byte, maxBlockSize)
	for i := range short {
		short[i] = period[i%len(period)]
	}
	out["short-period"] = short

	// Long-distance repetition: a long incompressible prefix followed by a
	// copy of its beginning, so the useful matches are tens of KiB away.
	long := make([]byte, maxBlockSize)
	rng := rand.New(rand.NewSource(4))
	rng.Read(long)
	copy(long[maxBlockSize/2:], long[:maxBlockSize/2])
	out["long-distance"] = long

	// Incompressible: pseudo-random bytes over the full alphabet.
	noise := make([]byte, maxBlockSize)
	rng2 := rand.New(rand.NewSource(5))
	rng2.Read(noise)
	out["incompressible"] = noise
	return out
}

func TestUnitStream(t *testing.T) {
	t.Run("ResourceGrowth", func(t *testing.T) {
		// Memory is O(n) expressed as a fixed set of whole-array makes, so the
		// allocation count is a small constant independent of n rather than
		// growing per position. Time must stay linear as well: 8x more data may
		// not cost anywhere near 8x^2 work.
		rng := rand.New(rand.NewSource(9))
		measure := func(n int) (float64, time.Duration) {
			src := make([]byte, n)
			rng.Read(src)
			allocs := testing.AllocsPerRun(3, func() {
				out, err := EncodeBest(nil, src)
				if err != nil || len(out) == 0 {
					t.Fatalf("EncodeBest n=%d: err=%v", n, err)
				}
			})
			var best time.Duration
			for rep := 0; rep < 3; rep++ {
				start := time.Now()
				if _, err := EncodeBest(nil, src); err != nil {
					t.Fatal(err)
				}
				if d := time.Since(start); rep == 0 || d < best {
					best = d
				}
			}
			return allocs, best
		}
		aSmall, dSmall := measure(8192)
		aLarge, dLarge := measure(65536)
		t.Logf("allocs n=8192:%.0f n=65536:%.0f; best duration %v vs %v", aSmall, aLarge, dSmall, dLarge)
		if aLarge > aSmall+2 {
			t.Fatalf("allocation count grew with input size: %.0f vs %.0f", aLarge, aSmall)
		}
		if dLarge > 40*dSmall {
			t.Fatalf("time grew super-linearly for 8x input: %v vs %v", dLarge, dSmall)
		}
	})

	t.Run("LargeMaterialsRoundTripAndGrowth", func(t *testing.T) {
		for name, src := range largeMaterials() {
			t.Run(name, func(t *testing.T) {
				enc := checkEncodeBest(t, src)
				greedy := Encode(nil, src)
				if len(enc) > len(greedy) {
					t.Fatalf("EncodeBest longer than greedy: %d > %d", len(enc), len(greedy))
				}
				t.Logf("%s: n=%d best=%d greedy=%d", name, len(src), len(enc), len(greedy))

				// Framed stream through NewBestWriter with fragmented writes.
				var buf bytes.Buffer
				w := NewBestWriter(&buf)
				for i := 0; i < len(src); {
					step := 1 + (i*137)%4097
					if i+step > len(src) {
						step = len(src) - i
					}
					if _, err := w.Write(src[i : i+step]); err != nil {
						t.Fatalf("Write: %v", err)
					}
					i += step
				}
				if err := w.Close(); err != nil {
					t.Fatalf("Close: %v", err)
				}
				got, err := io.ReadAll(NewReader(bytes.NewReader(buf.Bytes())))
				if err != nil || !bytes.Equal(got, src) {
					t.Fatalf("framed round trip: err=%v", err)
				}
				// The framed output must use only standard chunk types.
				p := len(magicChunk)
				for p < buf.Len() {
					head := buf.Bytes()[p:]
					if len(head) < chunkHeaderSize {
						t.Fatalf("truncated chunk header")
					}
					ct := head[0]
					if ct != chunkTypeCompressedData && ct != chunkTypeUncompressedData {
						t.Fatalf("non-standard chunk type %#x", ct)
					}
					clen := int(head[1]) | int(head[2])<<8 | int(head[3])<<16
					p += chunkHeaderSize + clen
				}
			})
		}
	})

	t.Run("ChunkSplitting", func(t *testing.T) {
		// More than 64 KiB forces multiple chunks; pieces repeat across the
		// chunk boundary so copies cannot leak past a block edge.
		part := bytes.Repeat([]byte("boundary-"), 9000) // 72 KiB
		src := append(append([]byte{}, part...), part...)
		var buf bytes.Buffer
		w := NewBestWriter(&buf)
		if _, err := w.Write(src[:1000]); err != nil {
			t.Fatal(err)
		}
		if err := w.Flush(); err != nil {
			t.Fatal(err)
		}
		if _, err := w.Write(src[1000:]); err != nil {
			t.Fatal(err)
		}
		if err := w.Close(); err != nil {
			t.Fatal(err)
		}
		got, err := io.ReadAll(NewReader(bytes.NewReader(buf.Bytes())))
		if err != nil || !bytes.Equal(got, src) {
			t.Fatalf("split chunk round trip: err=%v", err)
		}
		// Count compressed/uncompressed data chunks: at least three chunks
		// (the flush plus the >64KiB remainder).
		nChunks := 0
		p := len(magicChunk)
		for p < buf.Len() {
			nChunks++
			clen := int(buf.Bytes()[p+1]) | int(buf.Bytes()[p+2])<<8 | int(buf.Bytes()[p+3])<<16
			p += chunkHeaderSize + clen
		}
		if nChunks < 3 {
			t.Fatalf("expected at least 3 data chunks, got %d", nChunks)
		}
	})

	t.Run("ResetReplayAndModeStickiness", func(t *testing.T) {
		src := bytes.Repeat([]byte("reset-replay "), 3000)
		var first, second bytes.Buffer
		w := NewBestWriter(&first)
		if _, err := w.Write(src[:12345]); err != nil {
			t.Fatal(err)
		}
		if err := w.Flush(); err != nil {
			t.Fatal(err)
		}
		// Reset must keep Best mode and replay onto the new writer.
		w.Reset(&second)
		if !w.best {
			t.Fatalf("Reset dropped Best encoding mode")
		}
		if _, err := w.Write(src); err != nil {
			t.Fatal(err)
		}
		if err := w.Close(); err != nil {
			t.Fatal(err)
		}
		got, err := io.ReadAll(NewReader(bytes.NewReader(second.Bytes())))
		if err != nil || !bytes.Equal(got, src) {
			t.Fatalf("post-Reset replay: err=%v", err)
		}
		// Other constructors must not switch into Best mode.
		if NewWriter(nil).best || NewBufferedWriter(nil).best {
			t.Fatalf("default constructors unexpectedly enabled best mode")
		}
	})

	t.Run("StickyUnderlyingError", func(t *testing.T) {
		fw := &failingWriter{failAfter: 3}
		w := NewBestWriter(fw)
		var err error
		for err == nil {
			_, err = w.Write(bytes.Repeat([]byte("xy"), 5000))
		}
		if err == nil || err == errClosed {
			t.Fatalf("expected the underlying writer error, got %v", err)
		}
		sticky := err
		if _, err := w.Write([]byte("z")); err != sticky {
			t.Fatalf("error not sticky: got %v want %v", err, sticky)
		}
		if err := w.Flush(); err != sticky {
			t.Fatalf("Flush did not propagate sticky error")
		}
		if err := w.Close(); err != sticky {
			t.Fatalf("Close did not propagate sticky error")
		}
	})

	t.Run("SmallWritesAndUncompressedThreshold", func(t *testing.T) {
		// Incompressible chunk: the 12.5% rule must still select an
		// uncompressed chunk even under EncodeBest.
		noise := make([]byte, 40000)
		rand.New(rand.NewSource(6)).Read(noise)
		var buf bytes.Buffer
		w := NewBestWriter(&buf)
		for i := 0; i < len(noise); i += 7 {
			end := i + 7
			if end > len(noise) {
				end = len(noise)
			}
			if _, err := w.Write(noise[i:end]); err != nil {
				t.Fatal(err)
			}
		}
		if err := w.Close(); err != nil {
			t.Fatal(err)
		}
		sawUncompressed := false
		p := len(magicChunk)
		for p < buf.Len() {
			if buf.Bytes()[p] == chunkTypeUncompressedData {
				sawUncompressed = true
			}
			clen := int(buf.Bytes()[p+1]) | int(buf.Bytes()[p+2])<<8 | int(buf.Bytes()[p+3])<<16
			p += chunkHeaderSize + clen
		}
		if !sawUncompressed {
			t.Fatalf("incompressible chunk was not stored uncompressed")
		}
		got, err := io.ReadAll(NewReader(bytes.NewReader(buf.Bytes())))
		if err != nil || !bytes.Equal(got, noise) {
			t.Fatalf("uncompressed-chunk round trip: err=%v", err)
		}
	})
}

type failingWriter struct {
	failAfter int
	n         int
}

func (f *failingWriter) Write(p []byte) (int, error) {
	f.n++
	if f.n > f.failAfter {
		return 0, errors.New("synthetic underlying failure")
	}
	return len(p), nil
}
