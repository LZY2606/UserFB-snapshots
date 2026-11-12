// Copyright 2015, Joe Tsai. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.md file.

package bzip2

import (
	"github.com/dsnet/compress/internal"
	"github.com/dsnet/compress/internal/errors"
	"github.com/dsnet/compress/internal/prefix"
)

// blockDecoder holds the reusable workspaces needed to parse a single bzip2
// block: the bit reader, the Huffman/MTF/BWT transform state, and their
// scratch buffers. Both the sequential Reader and the random-access indexed
// reader drive this same decoder so that the format parsing logic (prefix
// codes, MTF/RLE2, BWT) exists in only one place.
type blockDecoder struct {
	rd prefixReader

	mtf moveToFront
	bwt burrowsWheelerTransform

	// These fields are allocated once and re-used for every block.
	treeSels []uint8
	codes2D  [maxNumTrees][maxNumSyms]prefix.PrefixCode
	codes1D  [maxNumTrees]prefix.PrefixCodes
	trees1D  [maxNumTrees]prefix.Decoder
	syms     []uint16
}

// decodeBlock parses one bzip2 block starting immediately after the 48-bit
// block magic, which the caller must have already consumed and validated. It
// performs the prefix, MTF/RLE2, and BWT stages and returns the post-BWT
// (pre-RLE1) byte buffer together with the block CRC stored in the header.
func (dec *blockDecoder) decodeBlock(level int) (buf []byte, blkCRC uint32) {
	blkCRC = uint32(dec.rd.ReadBitsBE64(32))
	if dec.rd.ReadBitsBE64(1) != 0 {
		panicf(errors.Deprecated, "block randomization is not supported")
	}

	// Read BWT related fields.
	ptr := int(dec.rd.ReadBitsBE64(24)) // BWT origin pointer

	// Read MTF related fields.
	var dictArr [256]uint8
	dict := dictArr[:0]
	bmapHi := uint16(dec.rd.ReadBits(16))
	for i := 0; i < 256; i, bmapHi = i+16, bmapHi>>1 {
		if bmapHi&1 > 0 {
			bmapLo := uint16(dec.rd.ReadBits(16))
			for j := 0; j < 16; j, bmapLo = j+1, bmapLo>>1 {
				if bmapLo&1 > 0 {
					dict = append(dict, uint8(i+j))
				}
			}
		}
	}

	// Step 1: Prefix encoding.
	syms := dec.decodePrefix(len(dict), level)

	// Step 2: Move-to-front transform and run-length encoding.
	dec.mtf.Init(dict, level*blockSize)
	buf = dec.mtf.Decode(syms)

	// Step 3: Burrows-Wheeler transformation.
	if ptr >= len(buf) {
		panicf(errors.Corrupted, "origin pointer (0x%06x) exceeds block size: %d", ptr, len(buf))
	}
	dec.bwt.Decode(buf, ptr)

	return buf, blkCRC
}

func (dec *blockDecoder) decodePrefix(numSyms, level int) (syms []uint16) {
	numSyms += 2 // Remove 0 symbol, add RUNA, RUNB, and EOF symbols
	if numSyms < 3 {
		panicf(errors.Corrupted, "not enough prefix symbols: %d", numSyms)
	}

	// Read information about the trees and tree selectors.
	var mtf internal.MoveToFront
	numTrees := int(dec.rd.ReadBitsBE64(3))
	if numTrees < minNumTrees || numTrees > maxNumTrees {
		panicf(errors.Corrupted, "invalid number of prefix trees: %d", numTrees)
	}
	numSels := int(dec.rd.ReadBitsBE64(15))
	if cap(dec.treeSels) < numSels {
		dec.treeSels = make([]uint8, numSels)
	}
	treeSels := dec.treeSels[:numSels]
	for i := range treeSels {
		sym, ok := dec.rd.TryReadSymbol(&decSel)
		if !ok {
			sym = dec.rd.ReadSymbol(&decSel)
		}
		if int(sym) >= numTrees {
			panicf(errors.Corrupted, "invalid prefix tree selector: %d", sym)
		}
		treeSels[i] = uint8(sym)
	}
	mtf.Decode(treeSels)
	dec.treeSels = treeSels

	// Initialize prefix codes.
	for i := range dec.codes2D[:numTrees] {
		dec.codes1D[i] = dec.codes2D[i][:numSyms]
	}
	dec.rd.ReadPrefixCodes(dec.codes1D[:numTrees], dec.trees1D[:numTrees])

	// Read prefix encoded symbols of compressed data.
	var tree *prefix.Decoder
	var blkLen, selIdx int
	syms = dec.syms[:0]
	for {
		if blkLen == 0 {
			blkLen = numBlockSyms
			if selIdx >= len(treeSels) {
				panicf(errors.Corrupted, "not enough prefix tree selectors")
			}
			tree = &dec.trees1D[treeSels[selIdx]]
			selIdx++
		}
		blkLen--
		sym, ok := dec.rd.TryReadSymbol(tree)
		if !ok {
			sym = dec.rd.ReadSymbol(tree)
		}

		if int(sym) == numSyms-1 {
			break // EOF marker
		}
		if int(sym) >= numSyms {
			panicf(errors.Corrupted, "invalid prefix symbol: %d", sym)
		}
		if len(syms) >= level*blockSize {
			panicf(errors.Corrupted, "number of prefix symbols exceeds block size")
		}
		syms = append(syms, uint16(sym))
	}
	dec.syms = syms
	return syms
}
