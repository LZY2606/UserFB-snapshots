// Copyright 2015, Joe Tsai. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.md file.

// Package bzip2 implements the BZip2 compressed data format.
//
// Canonical C implementation:
//
//	http://bzip.org
//
// Unofficial format specification:
//
//	https://github.com/dsnet/compress/blob/master/doc/bzip2-format.pdf
package bzip2

import (
	"fmt"
	"hash/crc32"
	"math/bits"

	"github.com/dsnet/compress/internal"
	"github.com/dsnet/compress/internal/errors"
)

// There does not exist a formal specification of the BZip2 format. As such,
// much of this work is derived by either reverse engineering the original C
// source code or using secondary sources.
//
// Significant amounts of fuzz testing is done to ensure that outputs from
// this package is properly decoded by the C library. Furthermore, we test that
// both this package and the C library agree about what inputs are invalid.
//
// Compression stack:
//	Run-length encoding 1     (RLE1)
//	Burrows-Wheeler transform (BWT)
//	Move-to-front transform   (MTF)
//	Run-length encoding 2     (RLE2)
//	Prefix encoding           (PE)
//
// References:
//	http://bzip.org/
//	https://en.wikipedia.org/wiki/Bzip2
//	https://code.google.com/p/jbzip2/

const (
	BestSpeed          = 1
	BestCompression    = 9
	DefaultCompression = 6
)

const (
	hdrMagic = 0x425a         // Hex of "BZ"
	blkMagic = 0x314159265359 // BCD of PI
	endMagic = 0x177245385090 // BCD of sqrt(PI)

	blockSize = 100000
)

func errorf(c int, f string, a ...interface{}) error {
	return errors.Error{Code: c, Pkg: "bzip2", Msg: fmt.Sprintf(f, a...)}
}

func panicf(c int, f string, a ...interface{}) {
	errors.Panic(errorf(c, f, a...))
}

// errWrap converts a lower-level errors.Error to be one from this package.
// The replaceCode passed in will be used to replace the code for any errors
// with the errors.Invalid code.
//
// For the Reader, set this to errors.Corrupted.
// For the Writer, set this to errors.Internal.
func errWrap(err error, replaceCode int) error {
	if cerr, ok := err.(errors.Error); ok {
		if errors.IsInvalid(cerr) {
			cerr.Code = replaceCode
		}
		err = errorf(cerr.Code, "%s", cerr.Msg)
	}
	return err
}

var errClosed = errorf(errors.Closed, "")

// crc computes the CRC-32 used by BZip2.
//
// The CRC-32 computation in bzip2 treats bytes as having bits in big-endian
// order. That is, the MSB is read before the LSB. Thus, we can use the
// standard library version of CRC-32 IEEE with some minor adjustments.
//
// The byte array is used as an intermediate buffer to swap the bits of every
// byte of the input.
type crc struct {
	val uint32
	buf [256]byte
}

// update computes the CRC-32 of appending buf to c.
func (c *crc) update(buf []byte) {
	cval := internal.ReverseUint32(c.val)
	for len(buf) > 0 {
		n := len(buf)
		if n > len(c.buf) {
			n = len(c.buf)
		}
		for i, b := range buf[:n] {
			c.buf[i] = internal.ReverseLUT[b]
		}
		cval = crc32.Update(cval, crc32.IEEETable, c.buf[:n])
		buf = buf[n:]
	}
	c.val = internal.ReverseUint32(cval)
}

// linOp is a linear map over GF(2)^32 stored as the images of the 32 unit
// basis vectors: applying it to s XORs op[i] for every set bit i of s.
type linOp [32]uint32

func linApply(op linOp, s uint32) (r uint32) {
	for s != 0 {
		i := bits.TrailingZeros32(s)
		r ^= op[i]
		s &= s - 1
	}
	return r
}

// crcByteOp is the linear part A of one reflected CRC-32 byte transition:
// A(s) = (s >> 8) xor IEEETable[s & 0xff].
var crcByteOp = func() (op linOp) {
	for i := 8; i < 32; i++ {
		op[i] = uint32(1) << uint(i-8)
	}
	for i := 0; i < 8; i++ {
		op[i] = crc32.IEEETable[1<<uint(i)]
	}
	return op
}()

// crcPowOp[k] is A^(2^k), the byte transition raised to a power of two. It
// depends only on the fixed CRC polynomial, never on the data, so every run
// CRC update reuses these precomputed operators.
var crcPowOp = func() (pow [63]linOp) {
	pow[0] = crcByteOp
	for k := 1; k < len(pow); k++ {
		for i := range pow[k] {
			pow[k][i] = linApply(pow[k-1], pow[k-1][i])
		}
	}
	return pow
}()

// updateRun advances the CRC-32 as if n copies of byte b had been appended,
// without ever materializing those bytes. The raw CRC recurrence is affine
// over GF(2), T(t) = A(t) xor D(b); this package chains crc32.Update calls,
// whose running state is the complement of the raw state, so the same linear
// operator A is used behind complement wrappers. Repeated application is
// evaluated by binary exponentiation in O(log n) operator applications, so a
// long RLE1 run costs no more work than a short one. The number of doubling
// levels is returned for use by the package-internal test hooks.
func (c *crc) updateRun(b byte, n int64) int {
	if n <= 0 {
		return 0
	}
	t := ^internal.ReverseUint32(c.val)
	curD := crc32.IEEETable[internal.ReverseLUT[b]]
	steps := 0
	for k := 0; n > 0; k++ {
		if n&1 != 0 {
			t = linApply(crcPowOp[k], t) ^ curD
		}
		n >>= 1
		if n > 0 {
			// T^(2m)(s) = A^m(T^m(s)); the constant term doubles as
			// D_(2m) = A^m(D_m) xor D_m.
			curD = linApply(crcPowOp[k], curD) ^ curD
			steps++
		}
	}
	c.val = internal.ReverseUint32(^t)
	return steps
}
