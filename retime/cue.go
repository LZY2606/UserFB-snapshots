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
	"encoding/binary"
	"fmt"

	"github.com/Comcast/gots/v3"
	"github.com/Comcast/gots/v3/packet"
)

const (
	scte35TableID          = 0xFC
	scte35CRCSize          = 4
	scte35MinSectionLength = 17 // splice_null, empty descriptor loop: 13 static bytes + CRC
	scte35MaxSectionLength = 4093
	scte35EncryptedBit     = 0x80
)

// sectionAssembler reassembles plaintext SCTE-35 sections on one cue PID.
// Sections are edited in place inside the output packets: only
// pts_adjustment changes and the MPEG-2 CRC is recomputed; the splice command
// times, break_duration, descriptors and padding keep their original bytes.
//
// A PUSI payload lays out as [pointer_field][pointer prefix][new sections].
// The pointer prefix finishes the previous section group; the pointer_field
// byte itself is neither a section byte nor stuffing, so it is left as a hole
// in the logical stream and never read.
type sectionAssembler struct {
	chunkStream
	started    bool
	cursor     int // logical offset of the next unprocessed section byte
	groupStart int // logical offset where the current section group begins
	startIndex int
}

func feedCue(index, pid int, pkt *packet.Packet, assemblers map[int]*sectionAssembler, delta90k int64) error {
	// A scrambled cue PID cannot carry a readable SCTE-35 section.
	if pkt[3]>>6 != 0 {
		return fmt.Errorf("retime: scrambled payload on cue PID %d at packet index %d is not supported", pid, index)
	}
	start, end, ok := payloadBounds(pkt)
	if !ok {
		return nil
	}
	payloadLen := end - start
	st := assemblers[pid]
	if st == nil {
		st = &sectionAssembler{}
		assemblers[pid] = st
	}

	if packet.PayloadUnitStartIndicator(pkt) {
		if payloadLen == 0 {
			return fmt.Errorf("retime: cue PID %d at packet index %d: PUSI packet has no room for a pointer_field", pid, index)
		}
		pointerField := int(pkt[start])
		if pointerField+1 > payloadLen {
			return fmt.Errorf("retime: cue PID %d at packet index %d: pointer_field %d runs past payload", pid, index, pointerField)
		}
		pos := st.length
		boundary := pos + pointerField

		if st.started {
			// The pointer prefix carries the tail of the previous section group.
			st.addAt(pos, pkt, start+1, start+1+pointerField)
			if err := st.processThrough(boundary, delta90k); err != nil {
				return fmt.Errorf("retime: cue PID %d at packet index %d: %w", pid, index, err)
			}
			if err := st.expectStuffing(boundary); err != nil {
				return fmt.Errorf("retime: cue PID %d at packet index %d: %w", pid, index, err)
			}
		} else {
			// No previous section exists; the prefix is padding but is kept in
			// place untouched like every other non-time byte.
			st.addAt(pos, pkt, start+1, start+1+pointerField)
		}

		// The pointer_field byte is a one byte hole; new sections follow it.
		st.groupStart = boundary + 1
		st.cursor = st.groupStart
		st.addAt(st.groupStart, pkt, start+1+pointerField, end)
		st.started = true
		st.startIndex = index
	} else {
		if !st.started {
			return fmt.Errorf("retime: cue PID %d payload at packet index %d has no section start (missing PUSI)", pid, index)
		}
		st.add(pkt, start, end)
	}

	if err := st.processOpen(delta90k); err != nil {
		return fmt.Errorf("retime: cue PID %d at packet index %d: %w", pid, index, err)
	}
	return nil
}

// processThrough edits every complete section at or before boundary. A section
// whose declared end passes boundary is illegal because the pointer_field
// promises the previous group finishes inside the prefix.
func (a *sectionAssembler) processThrough(boundary int, delta90k int64) error {
	for a.cursor < boundary {
		if a.byteAt(a.cursor) == 0xFF {
			return a.expectStuffing(boundary)
		}
		if a.cursor+3 > boundary {
			return fmt.Errorf("SCTE-35 section header is truncated inside pointer_field prefix")
		}
		sectionEnd, err := a.sectionEnd(boundary)
		if err != nil {
			return err
		}
		if sectionEnd > boundary {
			return fmt.Errorf("SCTE-35 section overruns its pointer_field boundary")
		}
		if err := a.retimeSection(a.cursor, sectionEnd, delta90k); err != nil {
			return err
		}
		a.cursor = sectionEnd
	}
	return nil
}

// processOpen edits sections as they complete; an incomplete section stays
// pending for the next continuation packet.
func (a *sectionAssembler) processOpen(delta90k int64) error {
	for a.cursor < a.length {
		if a.byteAt(a.cursor) == 0xFF {
			// Everything from here to the currently available end must be
			// stuffing; the next PUSI boundary check enforces the same rule
			// across the rest of the group.
			return a.expectStuffing(a.length)
		}
		if a.cursor+3 > a.length {
			return nil // wait for the section length bytes
		}
		sectionEnd, err := a.sectionEnd(a.length)
		if err != nil {
			return err
		}
		if sectionEnd > a.length {
			return nil // wait for the rest of the section
		}
		if err := a.retimeSection(a.cursor, sectionEnd, delta90k); err != nil {
			return err
		}
		a.cursor = sectionEnd
	}
	return nil
}

// sectionEnd returns the logical offset just past the section beginning at
// cursor, validating the declared section_length against available bytes.
func (a *sectionAssembler) sectionEnd(available int) (int, error) {
	sectionLength := int(uint16(a.byteAt(a.cursor+1)&0x03)<<8 | uint16(a.byteAt(a.cursor+2)))
	if sectionLength < scte35MinSectionLength || sectionLength > scte35MaxSectionLength {
		return 0, fmt.Errorf("SCTE-35 section_length %d is outside the legal range", sectionLength)
	}
	return a.cursor + 3 + sectionLength, nil
}

// expectStuffing verifies every byte from cursor to limit is 0xFF padding and
// advances the cursor to limit.
func (a *sectionAssembler) expectStuffing(limit int) error {
	for off := a.cursor; off < limit; off++ {
		if a.byteAt(off) != 0xFF {
			return fmt.Errorf("non-0xFF byte 0x%02x found in SCTE-35 stuffing at logical offset %d", a.byteAt(off), off)
		}
	}
	a.cursor = limit
	return nil
}

// truncated reports whether a section has started but not fully arrived.
func (a *sectionAssembler) truncated() bool {
	return a.cursor < a.length && a.byteAt(a.cursor) != 0xFF
}

// retimeSection validates one SCTE-35 section, shifts pts_adjustment in the
// 2^33 domain and recomputes the trailing CRC. The 33 bit command times,
// break_duration, descriptors and padding are deliberately left untouched.
func (a *sectionAssembler) retimeSection(sectionStart, sectionEnd int, delta90k int64) error {
	if a.byteAt(sectionStart) != scte35TableID {
		return fmt.Errorf("unsupported table_id 0x%02x on cue PID, expected 0xFC", a.byteAt(sectionStart))
	}
	flags := a.byteAt(sectionStart + 1)
	if flags&0x80 != 0 {
		return fmt.Errorf("section_syntax_indicator must be zero for SCTE-35")
	}
	if flags&0x30 != 0x30 {
		return fmt.Errorf("reserved bits in SCTE-35 section header are not set to 11")
	}
	if a.byteAt(sectionStart+4)&scte35EncryptedBit != 0 {
		return gots.ErrSCTE35EncryptionUnsupported
	}

	sectionBytes := make([]byte, sectionEnd-sectionStart)
	for i := range sectionBytes {
		sectionBytes[i] = a.byteAt(sectionStart + i)
	}
	existingCRC := binary.BigEndian.Uint32(sectionBytes[len(sectionBytes)-scte35CRCSize:])
	actualCRC := binary.BigEndian.Uint32(gots.ComputeCRC(sectionBytes[:len(sectionBytes)-scte35CRCSize]))
	if existingCRC != actualCRC {
		return fmt.Errorf("SCTE-35 CRC mismatch: section carries 0x%08X, computed 0x%08X", existingCRC, actualCRC)
	}

	// pts_adjustment is 33 bits; its top bit shares the flags byte at
	// sectionStart+4. The upper seven bits (encryption flags) are preserved.
	adjOffset := sectionStart + 4
	current := (uint64(a.byteAt(adjOffset)&0x01) << 32) |
		uint64(a.byteAt(adjOffset+1))<<24 |
		uint64(a.byteAt(adjOffset+2))<<16 |
		uint64(a.byteAt(adjOffset+3))<<8 |
		uint64(a.byteAt(adjOffset+4))
	shifted := uint64(mod64(int64(current)+delta90k, maxPTS33))

	a.setByteAt(adjOffset, (a.byteAt(adjOffset)&0xFE)|byte((shifted>>32)&0x01))
	a.setByteAt(adjOffset+1, byte(shifted>>24))
	a.setByteAt(adjOffset+2, byte(shifted>>16))
	a.setByteAt(adjOffset+3, byte(shifted>>8))
	a.setByteAt(adjOffset+4, byte(shifted))

	for i := range sectionBytes {
		sectionBytes[i] = a.byteAt(sectionStart + i)
	}
	crc := gots.ComputeCRC(sectionBytes[:len(sectionBytes)-scte35CRCSize])
	crcOffset := sectionEnd - scte35CRCSize
	for i := 0; i < scte35CRCSize; i++ {
		a.setByteAt(crcOffset+i, crc[i])
	}
	return nil
}
