// Copyright 2015, Joe Tsai. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE.md file.

package bzip2

import (
	"io"

	"github.com/dsnet/compress/internal"
	"github.com/dsnet/compress/internal/errors"
)

type Reader struct {
	InputOffset  int64 // Total number of bytes read from underlying io.Reader
	OutputOffset int64 // Total number of bytes emitted from Read

	err      error
	level    int    // The current compression level
	rdHdrFtr int    // Number of times we read the stream header and footer
	blkCRC   uint32 // CRC-32 IEEE of each block (as stored)
	endCRC   uint32 // Checksum of all blocks using bzip2's custom method

	crc crc
	rle runLengthEncoding
	dec blockDecoder

	fuzzReader // Exported functionality when fuzz testing
}

type ReaderConfig struct {
	_ struct{} // Blank field to prevent unkeyed struct literals
}

func NewReader(r io.Reader, conf *ReaderConfig) (*Reader, error) {
	zr := new(Reader)
	zr.Reset(r)
	return zr, nil
}

func (zr *Reader) Reset(r io.Reader) error {
	*zr = Reader{
		dec: zr.dec,
		rle: zr.rle,
	}
	zr.dec.rd.Init(r)
	return nil
}

func (zr *Reader) Read(buf []byte) (int, error) {
	for {
		cnt, err := zr.rle.Read(buf)
		if err != rleDone && zr.err == nil {
			zr.err = err
		}
		if cnt > 0 {
			zr.crc.update(buf[:cnt])
			zr.OutputOffset += int64(cnt)
			return cnt, nil
		}
		if zr.err != nil || len(buf) == 0 {
			return 0, zr.err
		}

		// Read the next chunk.
		zr.dec.rd.Offset = zr.InputOffset
		func() {
			defer errors.Recover(&zr.err)
			if zr.rdHdrFtr%2 == 0 {
				// Check if we are already at EOF.
				if err := zr.dec.rd.PullBits(1); err != nil {
					if err == io.ErrUnexpectedEOF && zr.rdHdrFtr > 0 {
						err = io.EOF // EOF is okay if we read at least one stream
					}
					errors.Panic(err)
				}

				// Read stream header.
				if zr.dec.rd.ReadBitsBE64(16) != hdrMagic {
					panicf(errors.Corrupted, "invalid stream magic")
				}
				if ver := zr.dec.rd.ReadBitsBE64(8); ver != 'h' {
					if ver == '0' {
						panicf(errors.Deprecated, "bzip1 format is not supported")
					}
					panicf(errors.Corrupted, "invalid version: %q", ver)
				}
				lvl := int(zr.dec.rd.ReadBitsBE64(8)) - '0'
				if lvl < BestSpeed || lvl > BestCompression {
					panicf(errors.Corrupted, "invalid block size: %d", lvl*blockSize)
				}
				zr.level = lvl
				zr.rdHdrFtr++
			} else {
				// Check and update the CRC.
				if internal.GoFuzz {
					zr.updateChecksum(-1, zr.crc.val) // Update with value
					zr.blkCRC = zr.crc.val            // Suppress CRC failures
				}
				if zr.blkCRC != zr.crc.val {
					panicf(errors.Corrupted, "mismatching block checksum")
				}
				zr.endCRC = (zr.endCRC<<1 | zr.endCRC>>31) ^ zr.blkCRC
			}
			buf, blkCRC := zr.decodeBlock()
			zr.blkCRC = blkCRC
			zr.rle.Init(buf)
		}()
		if zr.InputOffset, err = zr.dec.rd.Flush(); zr.err == nil {
			zr.err = err
		}
		if zr.err != nil {
			zr.err = errWrap(zr.err, errors.Corrupted)
			return 0, zr.err
		}
	}
}

func (zr *Reader) Close() error {
	if zr.err == io.EOF || zr.err == errClosed {
		zr.rle.Init(nil) // Make sure future reads fail
		zr.err = errClosed
		return nil
	}
	return zr.err // Return the persistent error
}

// decodeBlock reads the block or footer magic and decodes the next block.
// A stream footer terminates the stream and returns a nil buffer.
func (zr *Reader) decodeBlock() (buf []byte, blkCRC uint32) {
	if magic := zr.dec.rd.ReadBitsBE64(48); magic != blkMagic {
		if magic == endMagic {
			endCRC := uint32(zr.dec.rd.ReadBitsBE64(32))
			if internal.GoFuzz {
				zr.updateChecksum(zr.dec.rd.BitsRead()-32, zr.endCRC)
				endCRC = zr.endCRC // Suppress CRC failures
			}
			if zr.endCRC != endCRC {
				panicf(errors.Corrupted, "mismatching stream checksum")
			}
			zr.endCRC = 0
			zr.dec.rd.ReadPads()
			zr.rdHdrFtr++
			return nil, 0
		}
		panicf(errors.Corrupted, "invalid block or footer magic")
	}

	zr.crc.val = 0
	if internal.GoFuzz {
		// Record the offset of the block checksum for fuzz rewriting.
		zr.updateChecksum(zr.dec.rd.BitsRead(), 0)
	}
	return zr.dec.decodeBlock(zr.level)
}
