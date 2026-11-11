/**
* Copyright 2016 Comcast Cable Communications Management, LLC
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
 */

package gaad

import (
	"fmt"

	"github.com/Comcast/gaad/bitreader"
)

// DSEEdit describes one edit to a data_stream_element (DSE) in an ADTS frame.
//
// Tag is the 0..15 element_instance_tag that identifies the DSE.
//
// Data is the replacement payload. A zero-length Data describes a real, empty
// DSE; it does not mean removal. Data may contain at most 510 bytes.
//
// Remove deletes the DSE with Tag. Removing a tag that is not present in the
// frame is an error.
//
// Align sets the data_byte_align_flag of the rewritten DSE. For existing DSEs
// that are not edited, the original flag is preserved.
type DSEEdit struct {
	Tag    uint8
	Data   []byte
	Remove bool
	Align  bool
}

const (
	maxDSEPayload      = 510 // 255 + max esc_count (255)
	adtsHeaderBits     = 56
	adtsHeaderBytes    = adtsHeaderBits / 8
	maxADTSFrameLength = (1 << 13) - 1
)

// RewriteDSE rewrites the data_stream_elements of a single, complete,
// CRC-free, AAC-LC ADTS frame with a single raw_data_block.
//
// Existing DSEs identified by an edit are replaced or removed in place. Tags
// without an existing DSE are appended immediately before ID_END in the order
// in which they appear in edits. All other syntax bits are preserved
// bit-for-bit. Byte alignment padding is regenerated from the new bit
// positions and filled with zero bits.
func RewriteDSE(frame []byte, edits []DSEEdit) ([]byte, error) {
	if err := validateDSEEdits(edits); err != nil {
		return nil, err
	}

	scan, err := scanADTSFrame(frame)
	if err != nil {
		return nil, err
	}

	for _, edit := range edits {
		count := scan.dseCounts[edit.Tag]
		if count > 1 {
			return nil, fmt.Errorf("gaad: DSE tag %d appears multiple times in the frame", edit.Tag)
		}
		if edit.Remove && count == 0 {
			return nil, fmt.Errorf("gaad: cannot remove DSE tag %d: not present in the frame", edit.Tag)
		}
	}

	// With no edits the contract is byte-for-byte identity. Returning a copy
	// also guarantees that the result does not alias the caller's buffer.
	if len(edits) == 0 {
		out := make([]byte, len(frame))
		copy(out, frame)
		return out, nil
	}

	editByTag := make(map[uint8]DSEEdit, len(edits))
	for _, edit := range edits {
		editByTag[edit.Tag] = edit
	}

	var body bitWriter
	for _, element := range scan.elements {
		switch element.id {
		case ID_DSE:
			if edit, ok := editByTag[element.dse.tag]; ok {
				if edit.Remove {
					continue
				}
				body.writeDSE(element.dse.tag, edit.Align, edit.Data)
			} else {
				body.writeExistingDSE(frame, element.dse)
			}
		case ID_PCE:
			if err := body.writePCE(frame, element); err != nil {
				return nil, err
			}
		default:
			body.copyBits(frame, element.start, element.end)
		}
	}

	// New tags are appended in edits order, immediately before ID_END.
	for _, edit := range edits {
		if scan.dseCounts[edit.Tag] == 0 && !edit.Remove {
			body.writeDSE(edit.Tag, edit.Align, edit.Data)
		}
	}

	body.writeBits(uint64(ID_END), 3)
	body.alignZero()

	bodyBytes := len(body.buf)
	frameLength := adtsHeaderBytes + bodyBytes
	if frameLength > maxADTSFrameLength {
		return nil, fmt.Errorf("gaad: rewritten frame length %d exceeds ADTS maximum %d", frameLength, maxADTSFrameLength)
	}

	out := make([]byte, frameLength)
	var header bitWriter
	for i := 0; i < adtsHeaderBits; i++ {
		if i >= 30 && i < 43 {
			// aac_frame_length is a 13-bit field starting at bit 30.
			header.writeBit(byte((uint64(frameLength) >> uint(42-i)) & 1))
		} else {
			header.writeBit((frame[i/8] >> uint(7-i%8)) & 1)
		}
	}
	header.alignZero()
	copy(out, header.buf)
	copy(out[adtsHeaderBytes:], body.buf)
	return out, nil
}

func validateDSEEdits(edits []DSEEdit) error {
	seen := make(map[uint8]bool, len(edits))
	for _, edit := range edits {
		if edit.Tag > 15 {
			return fmt.Errorf("gaad: DSE element_instance_tag %d is outside 0..15", edit.Tag)
		}
		if seen[edit.Tag] {
			return fmt.Errorf("gaad: DSE tag %d is edited more than once", edit.Tag)
		}
		seen[edit.Tag] = true
		if len(edit.Data) > maxDSEPayload {
			return fmt.Errorf("gaad: DSE payload length %d exceeds maximum %d", len(edit.Data), maxDSEPayload)
		}
	}
	return nil
}

// bitWriter is a small MSB-first bit writer used to rebuild a raw_data_block
// without touching the audio syntax that is being preserved.
type bitWriter struct {
	buf   []byte
	nbits uint64
}

func (w *bitWriter) writeBit(bit byte) {
	if w.nbits%8 == 0 {
		w.buf = append(w.buf, 0)
	}
	if bit&1 != 0 {
		w.buf[w.nbits/8] |= 1 << uint(7-w.nbits%8)
	}
	w.nbits++
}

func (w *bitWriter) writeBits(value uint64, n int) {
	for i := n - 1; i >= 0; i-- {
		w.writeBit(byte((value >> uint(i)) & 1))
	}
}

func (w *bitWriter) alignZero() {
	for w.nbits%8 != 0 {
		w.writeBit(0)
	}
}

func (w *bitWriter) copyBits(src []byte, start, end uint64) {
	for i := start; i < end; i++ {
		w.writeBit((src[i/8] >> uint(7-i%8)) & 1)
	}
}

func (w *bitWriter) writeCountEsc(count int) {
	if count >= 255 {
		w.writeBits(255, 8)
		w.writeBits(uint64(count-255), 8)
	} else {
		w.writeBits(uint64(count), 8)
	}
}

func (w *bitWriter) writeDSE(tag uint8, align bool, data []byte) {
	w.writeBits(uint64(ID_DSE), 3)
	w.writeBits(uint64(tag), 4)
	if align {
		w.writeBit(1)
	} else {
		w.writeBit(0)
	}
	w.writeCountEsc(len(data))
	if align {
		w.alignZero()
	}
	for _, b := range data {
		w.writeBits(uint64(b), 8)
	}
}

// writeExistingDSE re-emits an unedited DSE. Its Count/Esc_count representation
// is derived from the preserved total count, and its payload bits are copied
// verbatim from the source. Only the alignment padding may move.
func (w *bitWriter) writeExistingDSE(src []byte, dse *dseScan) {
	w.writeBits(uint64(ID_DSE), 3)
	w.writeBits(uint64(dse.tag), 4)
	if dse.align {
		w.writeBit(1)
	} else {
		w.writeBit(0)
	}
	w.writeCountEsc(int(dse.count))
	if dse.align {
		w.alignZero()
	}
	w.copyBits(src, dse.dataStart, dse.end)
}

// writePCE copies a PCE verbatim except for its byte_alignment() padding,
// which depends on the new bit position.
func (w *bitWriter) writePCE(src []byte, element frameElement) error {
	alignPosition := element.pce.alignPosition
	alignedPosition := alignToByte(alignPosition)
	if alignedPosition > element.end || alignPosition < element.start+3 {
		return fmt.Errorf("gaad: malformed program_config_element alignment")
	}
	w.copyBits(src, element.start, alignPosition)
	w.alignZero()
	w.copyBits(src, alignedPosition, element.end)
	return nil
}

type adtsHeader struct {
	profile              uint8
	samplingFrequencyIdx uint8
}

type dseScan struct {
	tag       uint8
	align     bool
	count     uint16
	dataStart uint64
	end       uint64
}

type frameElement struct {
	id    uint8
	start uint64
	end   uint64
	dse   *dseScan
	pce   *program_config_element
}

type frameScan struct {
	header    adtsHeader
	elements  []frameElement
	dseCounts map[uint8]int
}

func alignToByte(bit uint64) uint64 {
	return (bit + 7) & ^uint64(7)
}

// parseRewriteHeader reads and validates the fixed and variable ADTS headers.
// The rewrite entry point is deliberately strict: the frame must start with a
// syncword, have no CRC, describe one raw_data_block, use the LC profile, and
// declare exactly the supplied byte length.
func parseRewriteHeader(frame []byte) (adtsHeader, error) {
	var header adtsHeader
	if len(frame) < adtsHeaderBytes {
		return header, fmt.Errorf("gaad: frame is shorter than an ADTS header")
	}
	r := bitreader.NewBitReader(frame)
	sync, err := r.ReadBitsAsUInt16(12)
	if err != nil || sync != 0x0fff {
		return header, fmt.Errorf("gaad: frame does not start with an ADTS syncword")
	}
	if _, err = r.ReadBitsAsUInt8(1); err != nil { // ID
		return header, err
	}
	layer, err := r.ReadBitsAsUInt8(2)
	if err != nil {
		return header, err
	}
	if layer != 0 {
		return header, fmt.Errorf("gaad: ADTS layer %d is not supported", layer)
	}
	protectionAbsent, err := r.ReadBitAsBool()
	if err != nil {
		return header, err
	}
	if !protectionAbsent {
		return header, fmt.Errorf("gaad: ADTS frames with CRC protection are not supported")
	}
	profile, err := r.ReadBitsAsUInt8(2)
	if err != nil {
		return header, err
	}
	if profile != 1 { // object_type 2 == AAC-LC
		return header, fmt.Errorf("gaad: only AAC-LC ADTS frames are supported")
	}
	header.profile = profile + 1
	header.samplingFrequencyIdx, err = r.ReadBitsAsUInt8(4)
	if err != nil {
		return header, err
	}
	if header.samplingFrequencyIdx > 12 {
		return header, fmt.Errorf("gaad: sampling_frequency_index %d is out of range", header.samplingFrequencyIdx)
	}
	r.SkipBits(1)                                  // private
	if _, err = r.ReadBitsAsUInt8(3); err != nil { // channel_configuration
		return header, err
	}
	r.SkipBits(1) // original
	r.SkipBits(1) // home
	r.SkipBits(2) // copyright_id, copyright_id_start
	frameLength, err := r.ReadBitsAsUInt16(13)
	if err != nil {
		return header, err
	}
	r.SkipBits(11) // adts_buffer_fullness
	numRawDataBlocks, err := r.ReadBitsAsUInt8(2)
	if err != nil {
		return header, err
	}
	if numRawDataBlocks != 0 {
		return header, fmt.Errorf("gaad: ADTS frames with multiple raw_data_blocks are not supported")
	}
	if int(frameLength) != len(frame) {
		return header, fmt.Errorf("gaad: declared ADTS frame length %d does not match %d supplied bytes", frameLength, len(frame))
	}
	return header, nil
}

// scanADTSFrame walks the syntax elements to record the exact bit positions of
// DSEs, PCE alignments, and the end of the raw_data_block. No audio is
// re-quantized: the walk simply uses the existing syntax parser to find the
// element boundaries.
func scanADTSFrame(frame []byte) (*frameScan, error) {
	header, err := parseRewriteHeader(frame)
	if err != nil {
		return nil, err
	}

	r := bitreader.NewBitReader(frame)
	r.SkipBits(adtsHeaderBits)

	adts := &ADTS{
		reader:       r,
		Profile:      header.profile,
		sfi:          header.samplingFrequencyIdx,
		Frame_length: 1024,
	}

	scan := &frameScan{
		header:    header,
		dseCounts: make(map[uint8]int),
	}
	totalBits := uint64(len(frame)) * 8
	var previousID uint8

	for {
		if r.BitsLeft() < 3 {
			return nil, fmt.Errorf("gaad: raw_data_block ended without a complete ID_END")
		}
		start := r.BitOffset()
		id, err := r.ReadBitsAsUInt8(3)
		if err != nil {
			return nil, err
		}

		element := frameElement{id: id, start: start}
		switch id {
		case ID_SCE:
			_, err = adts.single_channel_element()
		case ID_CPE:
			_, err = adts.channel_pair_element()
		case ID_CCE:
			_, err = adts.coupling_channel_element()
		case ID_LFE:
			_, err = adts.lfe_channel_element()
		case ID_DSE:
			var dse *dseScan
			dse, err = scanDSESyntax(r)
			element.dse = dse
		case ID_PCE:
			element.pce = adts.program_config_element()
		case ID_FIL:
			_, err = adts.fill_element(previousID)
		case ID_END:
			// ID_END must be followed only by the final byte_alignment()
			// padding within the declared frame.
			end := r.BitOffset()
			if alignToByte(end) != totalBits {
				return nil, fmt.Errorf("gaad: ID_END does not line up with the declared frame length")
			}
			return scan, nil
		default:
			return nil, fmt.Errorf("gaad: unsupported syntactic element %d", id)
		}
		if err != nil {
			return nil, fmt.Errorf("gaad: parsing syntactic element %d: %w", id, err)
		}

		end := r.BitOffset()
		element.end = end
		if end > totalBits {
			return nil, fmt.Errorf("gaad: syntactic element %d runs past the declared frame", id)
		}
		if end <= start+3 {
			// A valid element always has a payload after its 3-bit ID. No
			// progress here would otherwise loop forever on truncated input.
			return nil, fmt.Errorf("gaad: syntactic element %d was truncated", id)
		}

		if id == ID_PCE {
			alignPosition := element.pce.alignPosition
			alignedPosition := alignToByte(alignPosition)
			if alignPosition < start+3 || alignedPosition > end {
				return nil, fmt.Errorf("gaad: malformed program_config_element")
			}
			expected := alignedPosition + 8 + 8*uint64(element.pce.Comment_field_bytes)
			if end != expected {
				return nil, fmt.Errorf("gaad: program_config_element was truncated")
			}
		}
		if id == ID_DSE {
			scan.dseCounts[element.dse.tag]++
		}

		scan.elements = append(scan.elements, element)
		previousID = id
	}
}

// scanDSESyntax reads one data_stream_element strictly and returns its bit
// span. It rejects payloads that run past the supplied frame instead of
// borrowing bits from a following frame.
func scanDSESyntax(r *bitreader.BitReader) (*dseScan, error) {
	tag, err := r.ReadBitsAsUInt8(4)
	if err != nil {
		return nil, err
	}
	align, err := r.ReadBitAsBool()
	if err != nil {
		return nil, err
	}
	rawCount, err := r.ReadBitsAsUInt8(8)
	if err != nil {
		return nil, err
	}
	count := uint16(rawCount)
	if rawCount == 255 {
		escCount, err := r.ReadBitsAsUInt8(8)
		if err != nil {
			return nil, err
		}
		count += uint16(escCount)
	}
	if align {
		r.ByteAlign()
	}
	dataStart := r.BitOffset()
	if align && dataStart%8 != 0 {
		return nil, fmt.Errorf("gaad: data_stream_element byte alignment is truncated")
	}
	needed := uint64(count) * 8
	if uint64(r.BitsLeft()) < needed {
		return nil, fmt.Errorf("gaad: data_stream_element payload was truncated (need %d bytes, have %d bits)",
			count, r.BitsLeft())
	}
	if err := r.SkipBits(uint(needed)); err != nil {
		return nil, err
	}
	return &dseScan{
		tag:       tag,
		align:     align,
		count:     count,
		dataStart: dataStart,
		end:       r.BitOffset(),
	}, nil
}
