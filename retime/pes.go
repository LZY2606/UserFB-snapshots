/*
MIT License

Copyright 2016 Comcast Cable Communications Management, LLC

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

package retime

import (
	"fmt"

	"github.com/Comcast/gots/v3/packet"
)

// PES stream ids that carry no optional PES header fields. Such headers have
// no PTS/DTS to move and are passed through untouched.
var pesNoOptionalHeaderStreamIDs = map[byte]bool{
	0xBE: true, // padding stream
	0xBF: true, // private_stream_2
	0xF0: true, // ECM
	0xF1: true, // EMM
	0xF2: true, // DSM-CC
	0xF3: true, // ISO/IEC 13522
	0xF8: true, // ITU-T H.222.1 type E
	0xFF: true, // program_stream_directory
}

const (
	pesFixedHeaderSize = 9 // prefix(3) + stream_id(1) + length(2) + flags(2) + header_data_length(1)
)

// pesAssembler rebuilds the logical PES byte stream of one PID while keeping
// every byte anchored in its originating output packet.
type pesAssembler struct {
	chunkStream
	active     bool
	started    bool // a PUSI has ever opened a PES packet on this PID
	startIndex int  // packet index of the PUSI that opened the current header
}

func feedPES(index, pid int, pkt *packet.Packet, assemblers map[int]*pesAssembler, delta90k int64) error {
	// Scrambled elementary streams cannot be parsed as plaintext PES.
	if pkt[3]>>6 != 0 {
		return fmt.Errorf("retime: scrambled payload on PES PID %d at packet index %d is not supported", pid, index)
	}
	start, end, ok := payloadBounds(pkt)
	if !ok {
		return nil
	}
	st := assemblers[pid]
	if st == nil {
		st = &pesAssembler{}
		assemblers[pid] = st
	}

	if packet.PayloadUnitStartIndicator(pkt) {
		if st.active {
			return fmt.Errorf("retime: PES header on PID %d started at packet index %d is truncated before the next PUSI at packet index %d",
				pid, st.startIndex, index)
		}
		st.reset()
		st.add(pkt, start, end)
		st.active = true
		st.started = true
		st.startIndex = index
	} else {
		if !st.started {
			return fmt.Errorf("retime: PES PID %d payload at packet index %d has no PES header start (missing PUSI)", pid, index)
		}
		if !st.active {
			// Header already retimed; this packet continues the media body.
			return nil
		}
		st.add(pkt, start, end)
	}

	if err := st.processHeader(delta90k); err != nil {
		return fmt.Errorf("retime: PES PID %d at packet index %d: %w", pid, index, err)
	}
	if !st.active {
		// Header is fully handled; release the anchor list. Later payloads
		// belong to the media body and must pass through byte for byte.
		st.reset()
	}
	return nil
}

// processHeader parses as much of the PES header at the front of the buffered
// stream as is currently available and rewrites the timestamp fields once the
// header is complete. It returns nil while waiting for more packets.
func (a *pesAssembler) processHeader(delta90k int64) error {
	if a.length < 3 {
		return nil
	}
	if a.byteAt(0) != 0x00 || a.byteAt(1) != 0x00 || a.byteAt(2) != 0x01 {
		return fmt.Errorf("invalid PES packet_start_code_prefix")
	}
	if a.length < pesFixedHeaderSize {
		return nil
	}
	streamID := a.byteAt(3)
	if pesNoOptionalHeaderStreamIDs[streamID] {
		// No PTS/DTS can be present; the header and body pass through.
		a.active = false
		return nil
	}

	ptsDtsFlags := a.byteAt(7) >> 6
	headerDataLength := int(a.byteAt(8))
	headerEnd := pesFixedHeaderSize + headerDataLength
	if a.length < headerEnd {
		// The header spans more TS packets; wait for the rest.
		return nil
	}

	switch ptsDtsFlags {
	case 0: // no timestamps
	case 2: // PTS only; the 4 bit field prefix is '0010'.
		if err := validateTimeMarker(&a.chunkStream, 9, 0x20); err != nil {
			return fmt.Errorf("illegal PTS marker bits: %w", err)
		}
		a.writeShiftedTime(9, delta90k)
	case 3: // PTS and DTS; their prefixes are '0011' and '0001'.
		if err := validateTimeMarker(&a.chunkStream, 9, 0x30); err != nil {
			return fmt.Errorf("illegal PTS marker bits: %w", err)
		}
		if err := validateTimeMarker(&a.chunkStream, 14, 0x10); err != nil {
			return fmt.Errorf("illegal DTS marker bits: %w", err)
		}
		a.writeShiftedTime(9, delta90k)
		a.writeShiftedTime(14, delta90k)
	default: // value 1 is reserved by ISO/IEC 13818-1
		return fmt.Errorf("reserved PTS_DTS_flags value %d", ptsDtsFlags)
	}

	// The fixed plus optional header is complete; remaining buffered bytes are
	// media body and stay untouched. A zero PES_packet_length (video) is fine.
	a.active = false
	return nil
}

// validateTimeMarker checks the 4 bit prefix and the three trailing marker
// bits of a 5 byte PTS/DTS field at the given logical offset.
func validateTimeMarker(s *chunkStream, offset int, prefix byte) error {
	b0 := s.byteAt(offset)
	if b0&0xF1 != prefix|0x01 {
		return fmt.Errorf("bad leading marker byte 0x%02x", b0)
	}
	if s.byteAt(offset+2)&0x01 != 0x01 {
		return fmt.Errorf("bad middle marker bit")
	}
	if s.byteAt(offset+4)&0x01 != 0x01 {
		return fmt.Errorf("bad trailing marker bit")
	}
	return nil
}

// writeShiftedTime reads a 33 bit timestamp, applies delta90k in the 2^33
// domain and writes it back, preserving the field prefix and marker bits.
func (a *pesAssembler) writeShiftedTime(offset int, delta90k int64) {
	field := make([]byte, 5)
	for i := range field {
		field[i] = a.byteAt(offset + i)
	}
	current := extract33(field)
	shifted := mod64(int64(current)+delta90k, maxPTS33)
	prefix := field[0] & 0xF0
	field[0] = prefix | byte((shifted>>29)&0x07)<<1 | 0x01
	field[1] = byte(shifted >> 22)
	field[2] = byte(shifted>>14)&0xFE | 0x01
	field[3] = byte(shifted >> 7)
	field[4] = byte(shifted<<1) | 0x01
	for i, v := range field {
		a.setByteAt(offset+i, v)
	}
}

// extract33 decodes a 33 bit PTS/DTS value from a 5 byte timestamp field.
// The leading byte holds the 4 bit prefix above three time bits and a marker
// bit; byte two packs 7 time bits above a marker bit.
func extract33(b []byte) uint64 {
	return uint64((b[0]>>1)&0x07)<<30 |
		uint64(b[1])<<22 |
		uint64((b[2]>>1)&0x7F)<<15 |
		uint64(b[3])<<7 |
		uint64((b[4]>>1)&0x7F)
}
