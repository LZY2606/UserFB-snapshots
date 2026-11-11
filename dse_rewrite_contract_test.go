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
	"bytes"
	"fmt"
	"testing"
)

// The contract tests below use a completely independent bit writer to build
// both the input frames and the expected output frames. Positions of
// byte-alignment padding and the 13-bit ADTS length field are read back with
// plain bit accessors rather than through ParseADTS.

type contractBitWriter struct {
	buf []byte
	nb  int
}

func (w *contractBitWriter) put(v uint64, n int) {
	for i := n - 1; i >= 0; i-- {
		if w.nb%8 == 0 {
			w.buf = append(w.buf, 0)
		}
		if (v>>uint(i))&1 != 0 {
			w.buf[w.nb/8] |= 1 << uint(7-w.nb%8)
		}
		w.nb++
	}
}

func (w *contractBitWriter) align() {
	for w.nb%8 != 0 {
		w.put(0, 1)
	}
}

type contractDSE struct {
	tag   uint8
	align bool
	data  []byte
}

type contractPart struct {
	kind     string // "sce", "dse", "pce", or "fil"
	dse      contractDSE
	pceTag   uint8
	comment  []byte
	filCount uint8
}

type contractAlignment struct {
	start int
	end   int
}

type contractMeta struct {
	frame    []byte
	aligns   []contractAlignment
	countPos map[uint8]int
	escPos   map[uint8]int
}

// putContractSCE writes a fixed SCE with a non-empty spectral payload. The
// spectral section uses codebook 1; one 4-tuple is the nonzero codeword
// 10000 (values 1,0,0,0) and the remaining tuples are single-bit zero words.
// This keeps the "audio" side realistic enough that a rewriter which only
// handles empty spectral shells cannot pass.
func putContractSCE(w *contractBitWriter) {
	w.put(ID_SCE, 3)
	w.put(0, 4)   // element_instance_tag
	w.put(100, 8) // global_gain
	w.put(0, 1)   // ics_reserved_bit
	w.put(0, 2)   // ONLY_LONG_SEQUENCE
	w.put(0, 1)   // window_shape
	w.put(4, 6)   // max_sfb
	w.put(0, 1)   // predictor_data_present
	// section_data: one section, codebook 1, length 4 sfbs
	w.put(1, 4) // sect_cb
	w.put(4, 5) // sect_len_incr
	// scale_factor_data: one bit per sfbs (the zero bit decodes to a delta)
	for i := 0; i < 4; i++ {
		w.put(0, 1)
	}
	w.put(0, 1) // pulse_data_present
	w.put(0, 1) // tns_data_present
	w.put(0, 1) // gain_control_data_present
	// spectral_data: 16 coefficients = four codebook-1 4-tuples
	w.put(0x10, 5) // 10000 -> values 1,0,0,0
	w.put(0, 1)    // 0 -> 0,0,0,0
	w.put(0, 1)    // 0 -> 0,0,0,0
	w.put(0, 1)    // 0 -> 0,0,0,0
}

func putContractDSE(w *contractBitWriter, dse contractDSE, meta *contractMeta) {
	w.put(ID_DSE, 3)
	w.put(uint64(dse.tag), 4)
	if dse.align {
		w.put(1, 1)
	} else {
		w.put(0, 1)
	}
	meta.countPos[dse.tag] = w.nb + adtsHeaderBits
	if len(dse.data) >= 255 {
		w.put(255, 8)
		meta.escPos[dse.tag] = w.nb + adtsHeaderBits
		w.put(uint64(len(dse.data)-255), 8)
	} else {
		meta.escPos[dse.tag] = -1
		w.put(uint64(len(dse.data)), 8)
	}
	if dse.align {
		start := w.nb
		w.align()
		meta.aligns = append(meta.aligns, contractAlignment{start + adtsHeaderBits, w.nb + adtsHeaderBits})
	}
	for _, b := range dse.data {
		w.put(uint64(b), 8)
	}
}

// putContractFIL writes a fill_element carrying an EXT_FILL payload. Its bit
// span is deliberately not forced to a byte boundary; the rewriter preserves
// it as an opaque run of syntax bits.
func putContractFIL(w *contractBitWriter, count uint8) {
	w.put(ID_FIL, 3)
	w.put(uint64(count), 4)
	w.put(EXT_FILL, 4)
	w.put(0, 4) // fill_nibble
	for i := 0; i < int(count)-1; i++ {
		w.put(0, 8) // fill_byte
	}
}

func putContractPCE(w *contractBitWriter, tag uint8, comment []byte, meta *contractMeta) {
	w.put(ID_PCE, 3)
	w.put(uint64(tag), 4)
	w.put(2, 2) // object_type AAC-LC
	w.put(4, 4) // sampling_frequency_index
	w.put(1, 4) // num_front_channel_elements
	w.put(0, 4) // num_side_channel_elements
	w.put(0, 4) // num_back_channel_elements
	w.put(0, 2) // num_lfe_channel_elements
	w.put(0, 3) // num_assoc_data_elements
	w.put(0, 4) // num_valid_cc_elements
	w.put(0, 1) // mono_mixdown_present
	w.put(0, 1) // stereo_mixdown_present
	w.put(0, 1) // matrix_mixdown_idx_present
	w.put(0, 1) // front_element_is_cpe[0]
	w.put(0, 4) // front_element_tag_select[0]
	start := w.nb
	w.align()
	meta.aligns = append(meta.aligns, contractAlignment{start + adtsHeaderBits, w.nb + adtsHeaderBits})
	w.put(uint64(len(comment)), 8)
	for _, b := range comment {
		w.put(uint64(b), 8)
	}
}

func contractHeader(length int) []byte {
	var w contractBitWriter
	w.put(0x0fff, 12) // syncword
	w.put(0, 1)       // MPEG-4
	w.put(0, 2)       // layer
	w.put(1, 1)       // protection_absent
	w.put(1, 2)       // profile AAC-LC
	w.put(4, 4)       // sampling_frequency_index 44100
	w.put(0, 1)       // private
	w.put(2, 3)       // channel_configuration stereo
	w.put(0, 1)       // original
	w.put(0, 1)       // home
	w.put(0, 1)       // copyright_id
	w.put(0, 1)       // copyright_id_start
	w.put(uint64(length), 13)
	w.put(0x7ff, 11) // adts_buffer_fullness (VBR)
	w.put(0, 2)      // num_raw_data_blocks
	w.align()
	return w.buf
}

func assembleContractFrame(bodyBits []byte) []byte {
	length := adtsHeaderBytes + len(bodyBits)
	return append(contractHeader(length), bodyBits...)
}

func buildContractFrame(parts []contractPart) contractMeta {
	meta := contractMeta{
		countPos: make(map[uint8]int),
		escPos:   make(map[uint8]int),
	}
	var body contractBitWriter
	for _, part := range parts {
		switch part.kind {
		case "sce":
			putContractSCE(&body)
		case "dse":
			putContractDSE(&body, part.dse, &meta)
		case "pce":
			putContractPCE(&body, part.pceTag, part.comment, &meta)
		case "fil":
			putContractFIL(&body, part.filCount)
		default:
			panic("unknown contract part " + part.kind)
		}
	}
	body.put(ID_END, 3)
	start := body.nb
	body.align()
	meta.aligns = append(meta.aligns, contractAlignment{start + adtsHeaderBits, body.nb + adtsHeaderBits})
	meta.frame = assembleContractFrame(body.buf)
	return meta
}

func baseContractParts() []contractPart {
	return []contractPart{
		{kind: "sce"},
		{kind: "dse", dse: contractDSE{tag: 1, align: false, data: []byte("hello")}},
		{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
		{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
	}
}

func readContractBit(frame []byte, pos int) uint64 {
	return uint64((frame[pos/8] >> uint(7-pos%8)) & 1)
}

func readContractBits(frame []byte, pos, n int) uint64 {
	var v uint64
	for i := 0; i < n; i++ {
		v = (v << 1) | readContractBit(frame, pos+i)
	}
	return v
}

func readContractLength(frame []byte) int {
	return int(readContractBits(frame, 30, 13))
}

func setContractBit(frame []byte, pos int, value uint64) {
	mask := byte(1 << uint(7-pos%8))
	if value&1 != 0 {
		frame[pos/8] |= mask
	} else {
		frame[pos/8] &^= mask
	}
}

func patchContractLength(frame []byte, length int) []byte {
	out := append([]byte(nil), frame...)
	for i := 0; i < 13; i++ {
		setContractBit(out, 30+i, uint64(length)>>uint(12-i)&1)
	}
	return out
}

func callRewrite(t *testing.T, frame []byte, edits []DSEEdit) ([]byte, error) {
	t.Helper()
	frameCopy := append([]byte(nil), frame...)
	editCopy := make([]DSEEdit, len(edits))
	for i := range edits {
		editCopy[i] = edits[i]
		if edits[i].Data != nil {
			editCopy[i].Data = append([]byte(nil), edits[i].Data...)
		}
	}
	out, err := RewriteDSE(frame, edits)
	if !bytes.Equal(frame, frameCopy) {
		t.Fatal("RewriteDSE modified the input frame")
	}
	for i := range edits {
		if !bytes.Equal(edits[i].Data, editCopy[i].Data) {
			t.Fatalf("RewriteDSE modified edits[%d].Data", i)
		}
	}
	return out, err
}

// verifyContractOutput checks, using only raw bit accessors, that the ADTS
// length is the actual byte length and that every recorded alignment boundary
// is byte aligned and padded with zero bits.
func verifyContractOutput(t *testing.T, out []byte, meta contractMeta) {
	t.Helper()
	if len(out) < adtsHeaderBytes {
		t.Fatalf("rewritten frame is only %d bytes", len(out))
	}
	if got := readContractLength(out); got != len(out) {
		t.Fatalf("declared frame_length = %d, actual bytes = %d", got, len(out))
	}
	for _, a := range meta.aligns {
		if a.end%8 != 0 {
			t.Fatalf("alignment boundary %d is not byte aligned", a.end)
		}
		for pos := a.start; pos < a.end; pos++ {
			if readContractBit(out, pos) != 0 {
				t.Fatalf("alignment padding bit %d is not zero", pos)
			}
		}
	}
	for tag, pos := range meta.countPos {
		count := readContractBits(out, pos, 8)
		esc := meta.escPos[tag]
		if count == 255 {
			if esc < 0 {
				t.Fatalf("tag %d has escaped count but no esc_count position", tag)
			}
			escCount := readContractBits(out, esc, 8)
			if total := 255 + int(escCount); total > maxDSEPayload {
				t.Fatalf("tag %d count/esc_count totals %d", tag, total)
			}
		}
	}
}

func assertSameFrame(t *testing.T, got []byte, expected contractMeta) {
	t.Helper()
	if !bytes.Equal(got, expected.frame) {
		t.Fatalf("rewritten frame does not match the independently built expectation\n got: %x\nwant: %x", got, expected.frame)
	}
}

func patternBytes(n int) []byte {
	out := make([]byte, n)
	for i := range out {
		out[i] = byte((i*37 + 11) & 0xff)
	}
	return out
}

func TestDSERewriteContract(t *testing.T) {
	baseParts := baseContractParts()
	base := buildContractFrame(baseParts)

	// The fixed input really does carry nonzero spectral data.
	parsed, err := ParseADTS(base.frame)
	if err != nil {
		t.Fatalf("ParseADTS(base): %v", err)
	}
	nonzero := false
	for _, sce := range parsed.Single_channel_elements {
		if sce.Channel_stream != nil && sce.Channel_stream.Spectral_data != nil {
			for _, tuple := range sce.Channel_stream.Spectral_data.Hcod {
				for _, v := range tuple {
					if v != 0 {
						nonzero = true
					}
				}
			}
		}
	}
	if !nonzero {
		t.Fatal("contract input does not contain nonzero spectral data")
	}

	t.Run("identity", func(t *testing.T) {
		out, err := callRewrite(t, base.frame, nil)
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, base)
		verifyContractOutput(t, out, base)
		if &out[0] == &base.frame[0] {
			t.Fatal("output aliases the input")
		}
	})

	t.Run("delete non-aligned DSE shifts PCE", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
			{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
		}
		expected := buildContractFrame(parts)
		out, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 1, Remove: true}})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		verifyContractOutput(t, out, expected)

		// Deleting a DSE which is not present is an error.
		if _, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 9, Remove: true}}); err == nil {
			t.Fatal("deleting an absent tag must fail")
		}
	})

	t.Run("delete aligned DSE shifts ID_END alignment", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: false, data: []byte("hello")}},
			{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
		}
		expected := buildContractFrame(parts)
		out, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 2, Remove: true}})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		verifyContractOutput(t, out, expected)
	})

	t.Run("delete aligned DSE shifts an unedited fill element", func(t *testing.T) {
		inputParts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: true, data: []byte("hello")}},
			{kind: "fil", filCount: 5},
			{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
			{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
		}
		input := buildContractFrame(inputParts)
		parts := []contractPart{
			{kind: "sce"},
			{kind: "fil", filCount: 5},
			{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
			{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
		}
		expected := buildContractFrame(parts)
		out, err := callRewrite(t, input.frame, []DSEEdit{{Tag: 1, Remove: true}})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		verifyContractOutput(t, out, expected)
		if _, err := ParseADTS(out); err != nil {
			t.Fatalf("ParseADTS after shifting a fill element: %v", err)
		}
	})

	t.Run("align switch", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: true, data: []byte("hello")}},
			{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
			{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
		}
		expected := buildContractFrame(parts)
		out, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 1, Data: []byte("hello"), Align: true}})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		verifyContractOutput(t, out, expected)
	})

	t.Run("replace payload and size", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: false, data: []byte("xy")}},
			{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
			{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
		}
		expected := buildContractFrame(parts)
		out, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 1, Data: []byte("xy"), Align: false}})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		verifyContractOutput(t, out, expected)
	})

	t.Run("append in edits order", func(t *testing.T) {
		parts := append(baseContractParts(),
			contractPart{kind: "dse", dse: contractDSE{tag: 4, align: false, data: []byte("four")}},
			contractPart{kind: "dse", dse: contractDSE{tag: 3, align: true, data: []byte("three")}},
		)
		expected := buildContractFrame(parts)
		// The edits are deliberately supplied in the opposite tag order; the
		// appended elements must follow the edits order.
		out, err := callRewrite(t, base.frame, []DSEEdit{
			{Tag: 4, Data: []byte("four")},
			{Tag: 3, Data: []byte("three"), Align: true},
		})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		verifyContractOutput(t, out, expected)

		if _, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 1}, {Tag: 1}}); err == nil {
			t.Fatal("duplicate edit tags must fail")
		}
	})

	t.Run("payload sizes 254 255 256 510", func(t *testing.T) {
		for _, size := range []int{254, 255, 256, 510} {
			size := size
			t.Run(fmt.Sprintf("%d", size), func(t *testing.T) {
				parts := []contractPart{
					{kind: "sce"},
					{kind: "dse", dse: contractDSE{tag: 1, align: false, data: patternBytes(size)}},
					{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
					{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
				}
				expected := buildContractFrame(parts)
				out, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 1, Data: patternBytes(size)}})
				if err != nil {
					t.Fatalf("RewriteDSE: %v", err)
				}
				assertSameFrame(t, out, expected)
				verifyContractOutput(t, out, expected)

				countPos := expected.countPos[1]
				gotCount := int(readContractBits(out, countPos, 8))
				if size >= 255 {
					if gotCount != 255 {
						t.Fatalf("size %d: count = %d, want 255", size, gotCount)
					}
					gotEsc := int(readContractBits(out, expected.escPos[1], 8))
					if gotEsc != size-255 {
						t.Fatalf("size %d: esc_count = %d, want %d", size, gotEsc, size-255)
					}
				} else if gotCount != size {
					t.Fatalf("size %d: count = %d", size, gotCount)
				}

				// ParseADTS must also see the true total, including the
				// escaped form, without wrapping in a uint8.
				parsed, err := ParseADTS(out)
				if err != nil {
					t.Fatalf("ParseADTS: %v", err)
				}
				for _, e := range parsed.Data_stream_elements {
					if e.Element_instance_tag == 1 && int(e.Count) != size {
						t.Fatalf("size %d: ParseADTS Count = %d", size, e.Count)
					}
				}
			})
		}

		if _, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 1, Data: patternBytes(511)}}); err == nil {
			t.Fatal("a 511-byte DSE payload must fail")
		}
	})

	t.Run("empty payload is a real DSE", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: false}},
			{kind: "pce", pceTag: 6, comment: []byte{0xaa, 0xbb, 0xcc}},
			{kind: "dse", dse: contractDSE{tag: 2, align: true, data: []byte("tail")}},
		}
		expected := buildContractFrame(parts)
		out, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 1, Data: []byte{}}})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		verifyContractOutput(t, out, expected)
		if got := readContractBits(out, expected.countPos[1], 8); got != 0 {
			t.Fatalf("empty DSE count = %d, want 0", got)
		}

		parsed, err := ParseADTS(out)
		if err != nil {
			t.Fatalf("ParseADTS: %v", err)
		}
		for _, e := range parsed.Data_stream_elements {
			if e.Element_instance_tag == 1 && e.Count != 0 {
				t.Fatalf("empty DSE Count = %d, want 0", e.Count)
			}
		}
	})

	t.Run("existing 255/esc representation is preserved", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: true, data: patternBytes(255)}},
		}
		input := buildContractFrame(parts)
		expected := buildContractFrame(parts)
		out, err := callRewrite(t, input.frame, nil)
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		assertSameFrame(t, out, expected)
		if got := readContractBits(out, expected.countPos[1], 8); got != 255 {
			t.Fatalf("preserved count = %d, want 255", got)
		}
		if got := readContractBits(out, expected.escPos[1], 8); got != 0 {
			t.Fatalf("preserved esc_count = %d, want 0", got)
		}
	})

	t.Run("duplicate tag in frame", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: false, data: []byte("one")}},
			{kind: "dse", dse: contractDSE{tag: 1, align: true, data: []byte("two")}},
		}
		dup := buildContractFrame(parts)
		if _, err := callRewrite(t, dup.frame, []DSEEdit{{Tag: 1, Data: []byte("new")}}); err == nil {
			t.Fatal("editing a tag that appears multiple times must fail")
		}
		// Unedited duplicates are simply carried through.
		out, err := callRewrite(t, dup.frame, nil)
		if err != nil {
			t.Fatalf("identity with duplicate unedited tags: %v", err)
		}
		assertSameFrame(t, out, dup)
	})

	t.Run("invalid tags and edits", func(t *testing.T) {
		cases := []struct {
			name  string
			edits []DSEEdit
		}{
			{"tag out of range", []DSEEdit{{Tag: 16}}},
			{"duplicate edits", []DSEEdit{{Tag: 2}, {Tag: 2}}},
		}
		for _, tc := range cases {
			if _, err := callRewrite(t, base.frame, tc.edits); err == nil {
				t.Fatalf("%s: expected an error", tc.name)
			}
		}
	})

	t.Run("scope and length rejection", func(t *testing.T) {
		protected := append([]byte(nil), base.frame...)
		setContractBit(protected, 15, 0) // protection_absent = 0
		if _, err := callRewrite(t, protected, nil); err == nil {
			t.Fatal("protected frame must be rejected")
		}

		notLC := append([]byte(nil), base.frame...)
		setContractBit(notLC, 17, 0) // profile 00
		if _, err := callRewrite(t, notLC, nil); err == nil {
			t.Fatal("non-LC profile must be rejected")
		}

		multi := append([]byte(nil), base.frame...)
		setContractBit(multi, 55, 1) // num_raw_data_blocks = 1
		if _, err := callRewrite(t, multi, nil); err == nil {
			t.Fatal("multiple raw_data_blocks must be rejected")
		}

		if _, err := callRewrite(t, patchContractLength(base.frame, len(base.frame)+1), nil); err == nil {
			t.Fatal("declared/slice length mismatch must be rejected")
		}

		// A frame that declares its actual length but has no complete ID_END.
		var body contractBitWriter
		putContractSCE(&body)
		putContractDSE(&body, contractDSE{tag: 2, align: true, data: []byte("tail")}, &contractMeta{
			countPos: make(map[uint8]int),
			escPos:   make(map[uint8]int),
		})
		body.align()
		noEnd := assembleContractFrame(body.buf)
		if _, err := callRewrite(t, noEnd, nil); err == nil {
			t.Fatal("a frame without ID_END must be rejected")
		}

		// A DSE whose declared count exceeds the supplied bytes.
		var truncatedBody contractBitWriter
		putContractSCE(&truncatedBody)
		truncatedBody.put(ID_DSE, 3)
		truncatedBody.put(2, 4)
		truncatedBody.put(0, 1)  // align flag
		truncatedBody.put(10, 8) // count = 10
		truncatedBody.put(0x41, 8)
		truncatedBody.put(0x42, 8)
		truncatedBody.align()
		truncated := assembleContractFrame(truncatedBody.buf)
		if _, err := callRewrite(t, truncated, nil); err == nil {
			t.Fatal("a truncated DSE payload must be rejected")
		}

		// Truncating bytes from a valid frame also mismatches its declared length.
		if _, err := callRewrite(t, base.frame[:len(base.frame)-1], nil); err == nil {
			t.Fatal("truncated frame must be rejected")
		}
	})

	t.Run("rewritten length over 8191", func(t *testing.T) {
		parts := []contractPart{
			{kind: "sce"},
			{kind: "dse", dse: contractDSE{tag: 1, align: false, data: patternBytes(510)}},
		}
		input := buildContractFrame(parts)
		var edits []DSEEdit
		for tag := uint8(0); tag <= 15; tag++ {
			if tag == 1 {
				continue
			}
			edits = append(edits, DSEEdit{Tag: tag, Data: patternBytes(510)})
		}
		if _, err := callRewrite(t, input.frame, edits); err == nil {
			t.Fatal("a rewritten frame longer than 8191 bytes must fail")
		}
	})

	t.Run("output does not alias caller storage", func(t *testing.T) {
		out, err := callRewrite(t, base.frame, []DSEEdit{{Tag: 2, Remove: true}})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		inputCopy := append([]byte(nil), base.frame...)
		for i := range out {
			out[i] ^= 0xff
		}
		if !bytes.Equal(base.frame, inputCopy) {
			t.Fatal("mutating the output modified the input frame")
		}
	})

	t.Run("parse rewritten auxiliary content", func(t *testing.T) {
		out, err := callRewrite(t, base.frame, []DSEEdit{
			{Tag: 2, Data: []byte("new-tail"), Align: true},
			{Tag: 7, Data: []byte("added"), Align: false},
		})
		if err != nil {
			t.Fatalf("RewriteDSE: %v", err)
		}
		parsed, err := ParseADTS(out)
		if err != nil {
			t.Fatalf("ParseADTS: %v", err)
		}
		want := map[uint8]string{2: "new-tail", 7: "added"}
		for tag, data := range want {
			found := false
			for _, e := range parsed.Data_stream_elements {
				if e.Element_instance_tag == tag {
					found = true
					if string(e.Data_stream_byte[tag]) != data {
						t.Fatalf("tag %d data = %q, want %q", tag, string(e.Data_stream_byte[tag]), data)
					}
				}
			}
			if !found {
				t.Fatalf("tag %d not found after rewrite", tag)
			}
		}
	})
}
