// mpa, an MPEG-1 Audio library
// Copyright (C) 2014 KORÁNDI Zoltán <korandi.z@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License, version 3 as
// published by the Free Software Foundation.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

package mpa

import (
	"errors"
	"fmt"
	"io"
)

// A CRCError is returned by DecodeFrame in strict mode (see Decoder.CheckCRC)
// when the CRC check of a CRC-protected Layer I or Layer II frame fails.
//
// The whole frame has been consumed and its PCM samples have been discarded.
// The decoder is positioned at the beginning of the next frame, so the next
// call to DecodeFrame can continue with it. Since the rejected frame never
// reached the synthesis filterbank, it does not affect the decoding of
// subsequent frames.
type CRCError struct {
	// Stored is the crc_check value found in the frame.
	Stored uint16

	// Computed is the value computed over the CRC-protected parts of the
	// frame.
	Computed uint16
}

// Error returns the error as a string.
func (err CRCError) Error() string {
	return fmt.Sprintf("mpa: CRC check failed (stored 0x%04x, computed 0x%04x)",
		err.Stored, err.Computed)
}

// As allows errors.As to match a CRCError both directly and through a *CRCError
// target.
func (err CRCError) As(target interface{}) bool {
	if p, ok := target.(**CRCError); ok {
		e := err
		*p = &e
		return true
	}
	return false
}

// ErrUnsupportedCRC is returned by DecodeFrame in strict mode (see
// Decoder.CheckCRC) for CRC-protected frames whose CRC coverage is not defined
// by this package: Layer III frames and Layer I/II free format frames. These
// frames are not decoded in strict mode.
var ErrUnsupportedCRC = errors.New("mpa: CRC check not supported for this frame")

// crcAccumulator computes the MPEG-1 Audio CRC. The register is initialized to
// 0xffff, the polynomial is 0x8005, bits are fed MSB-first, and there is no
// final XOR.
type crcAccumulator struct {
	reg uint16
}

// newCRCAccumulator returns an accumulator initialized to the MPEG-1 Audio
// initial value, 0xffff.
func newCRCAccumulator() crcAccumulator {
	return crcAccumulator{reg: 0xffff}
}

// addBits feeds the n least significant bits of v into the accumulator, most
// significant bit first.
func (crc *crcAccumulator) addBits(v, n int) {
	for i := n - 1; i >= 0; i-- {
		bit := uint16((v >> uint(i)) & 1)
		feedback := (crc.reg>>15)&1 ^ bit
		crc.reg <<= 1
		if feedback != 0 {
			crc.reg ^= 0x8005
		}
	}
}

// readCRC reads the crc_check field of the current protected frame. Any
// truncation in the middle of the field is reported as io.ErrUnexpectedEOF.
func (d *Decoder) readCRC() error {
	var crc uint16
	for i := 0; i < 2; i++ {
		b, err := d.stream.readByte()
		if err == io.EOF {
			return io.ErrUnexpectedEOF
		} else if err != nil {
			return err
		}
		crc = crc<<8 | uint16(b)
	}
	d.crcStored = crc
	return nil
}

// checkCRC compares the accumulator with the stored crc_check value. It is a
// no-op unless strict checking is enabled for the current frame, and it only
// performs the comparison once per frame.
//
// On a mismatch, the rest of the frame is discarded so the decoder is left
// exactly at the beginning of the following frame, and a CRCError is returned.
func (d *Decoder) checkCRC() error {
	if !d.crcEnabled || d.crcChecked {
		return nil
	}
	d.crcChecked = true

	if d.crca.reg != d.crcStored {
		if err := d.skipFrameRest(); err != nil {
			return err
		}
		return CRCError{Stored: d.crcStored, Computed: d.crca.reg}
	}
	return nil
}

// frameBytes returns the total size of the current frame in bytes, including
// the header. Free format frames return 0.
func (d *Decoder) frameBytes() int {
	size := 12 * bitrateBps[d.layer][d.bitrateIndex]
	if d.layer != 1 {
		size *= 12
	}
	size /= samplingFreqHz[d.samplingFreq]
	size += d.paddingBit
	if d.layer == 1 {
		size *= 4
	}
	return size
}

// skipFrameRest discards the remainder of the current Layer I or Layer II
// frame. In strict mode this keeps the read position exact for frames whose
// payload is not CRC-protected, so that a following frame (or a false syncword
// within the ancillary data) is never mistaken for a continuation. The number
// of bytes consumed since the frame started is derived from the frame size and
// the stream position recorded in decodeHeader.
func (d *Decoder) skipFrameRest() error {
	size := d.frameBytes()
	if size == 0 {
		return nil
	}
	toSkip := size - d.consumedBytes()
	if toSkip <= 0 {
		return nil
	}
	if _, err := d.stream.skipBytes(toSkip); err != nil {
		if err == io.EOF {
			return io.ErrUnexpectedEOF
		}
		return err
	}
	return nil
}

// consumedBytes returns the number of bytes read from the stream since the
// current frame's header was found.
func (d *Decoder) consumedBytes() int {
	return d.stream.bytes - d.frameStartBytes
}

// readProtectedBits reads n bits from the stream and, in strict mode, feeds
// them into the CRC accumulator. It always returns the value read.
func (d *Decoder) readProtectedBits(n int) (int, error) {
	v, err := d.stream.readBits(n)
	if err != nil {
		return 0, err
	}
	if d.crcEnabled {
		d.crca.addBits(v, n)
	}
	return v, nil
}
