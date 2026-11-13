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
//
// Please note that, being hungarian, my last name comes before my first
// name. That's why it's in all caps, and not because I like to shout my
// name. So please don't start your emails with "Hi Korandi" or "Dear Mr.
// Zoltan", because it annoys the hell out of me. Thanks.

package mpa

import "errors"

// ErrUnsupportedCRC is returned by DecodeFrame in strict CRC mode
// (Decoder.CheckCRC is true) when the current frame carries a crc_check but
// the decoder cannot determine the protected region, so it refuses to guess.
// This happens with Layer III frames and with free-format bitrates.
var ErrUnsupportedCRC = errors.New("mpa: CRC check not supported for this frame")

// CRCError is returned by DecodeFrame in strict CRC mode
// (Decoder.CheckCRC is true) when a fully read, error-protected Layer I or
// Layer II frame fails its CRC check. The frame is discarded in its entirety,
// so neither the decoded samples nor the synthesis filter histories are
// updated, and the next call to DecodeFrame begins at the frame following the
// rejected one.
//
// Stored is the crc_check value read from the frame, Computed is the value
// calculated from the protected header and audio fields.
type CRCError struct {
	Stored   uint16
	Computed uint16
}

// Error implements the error interface.
func (err CRCError) Error() string {
	return "mpa: CRC check failed"
}

// crc16 computes the MPEG audio CRC: initial value 0xffff, generator
// polynomial 0x8005, MSB-first, no final XOR. Bits are fed in exactly the
// order in which they appear in the bitstream.
type crc16 struct {
	state uint16
}

// reset sets the accumulator back to its initial value.
func (c *crc16) reset() {
	c.state = 0xffff
}

// addBit feeds one bit (the MSB of b) to the accumulator.
func (c *crc16) addBit(b uint) {
	in := (c.state >> 15) ^ uint16(b&1)
	c.state <<= 1
	c.state ^= in * 0x8005
}

// addBits feeds the lowest n bits of value, MSB-first.
func (c *crc16) addBits(value uint, n int) {
	for i := n - 1; i >= 0; i-- {
		c.addBit(uint((value >> uint(i)) & 1))
	}
}

// addByte feeds eight bits of b, MSB-first.
func (c *crc16) addByte(b byte) {
	c.addBits(uint(b), 8)
}
