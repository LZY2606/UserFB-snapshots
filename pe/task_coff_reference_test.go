// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import (
	"bytes"
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"os"
	"path/filepath"
	"testing"

	"github.com/Binject/debug/task_support/fixture"
	"github.com/Binject/debug/task_support/taskcheck"
)

// IMAGE_REL_AMD64_ADDR64 is the AMD64 64-bit absolute relocation type.
const IMAGE_REL_AMD64_ADDR64 = 1

// ---- small on-disk COFF/BigObj builder (standard library only) ----

type testSection struct {
	name   string
	data   []byte
	relocs []Reloc
}

type testAux struct {
	raw []byte
}

type testSymbol struct {
	name    string
	value   uint32
	section int32
	typ     uint16
	class   uint8
	aux     []testAux
}

type builtCOFF struct {
	raw               []byte
	big               bool
	stride            uint32
	symbolTableOffset uint32
	stringTableOffset uint32
	// primarySlots[i] is the raw, aux-inclusive slot index of syms[i].
	primarySlots []uint32
	sectionData  []uint32
	sectionReloc []uint32
}

func align4(v uint32) uint32 { return (v + 3) &^ 3 }

// buildTestCOFF lays out a complete object. Big objects use the 56-byte
// BigObj v2 header and 20-byte slots; ordinary objects use the 20-byte
// header and 18-byte slots. Nothing here is ever executed or linked.
func buildTestCOFF(big bool, secs []testSection, syms []testSymbol) *builtCOFF {
	const (
		ordinaryHeader = 20
		sectionHeader  = 40
		relocRecord    = 10
	)
	le := binary.LittleEndian
	headerSize := uint32(ordinaryHeader)
	stride := uint32(COFFSymbolSize)
	if big {
		headerSize = bigObjHeaderSize
		stride = bigObjSymbolSize
	}

	// Assign string-table offsets for every long name.
	strings := map[string]uint32{}
	var stringBlob bytes.Buffer
	intern := func(name string) uint32 {
		if len(name) <= 8 {
			return 0
		}
		if off, ok := strings[name]; ok {
			return off
		}
		off := uint32(4 + stringBlob.Len())
		strings[name] = off
		stringBlob.WriteString(name)
		stringBlob.WriteByte(0)
		return off
	}
	for i := range secs {
		if len(secs[i].name) > 8 {
			intern(secs[i].name)
		}
	}
	for i := range syms {
		intern(syms[i].name)
	}

	// Raw slot indexes, auxiliary records included.
	primarySlots := make([]uint32, len(syms))
	var totalSlots uint32
	for i := range syms {
		primarySlots[i] = totalSlots
		totalSlots += 1 + uint32(len(syms[i].aux))
	}

	n := uint32(len(secs))
	cursor := headerSize + sectionHeader*n

	sectionData := make([]uint32, n)
	for i := range secs {
		if len(secs[i].data) > 0 {
			cursor = align4(cursor)
			sectionData[i] = cursor
			cursor += uint32(len(secs[i].data))
		}
	}
	sectionReloc := make([]uint32, n)
	for i := range secs {
		if len(secs[i].relocs) > 0 {
			cursor = align4(cursor)
			sectionReloc[i] = cursor
			cursor += relocRecord * uint32(len(secs[i].relocs))
		}
	}
	cursor = align4(cursor)
	symOff := cursor
	cursor += stride * totalSlots
	stringTableOff := cursor
	stringTableLen := uint32(4 + stringBlob.Len())
	cursor += stringTableLen

	out := make([]byte, cursor)
	putName := func(dst []byte, name string) {
		if len(name) <= 8 {
			copy(dst, name)
			return
		}
		le.PutUint32(dst[4:], strings[name])
	}
	putSectionName := func(dst []byte, name string) {
		if len(name) <= 8 {
			copy(dst, name)
			return
		}
		copy(dst, "/"+itoa(strings[name]))
	}

	if big {
		le.PutUint16(out[0:], bigObjSig1)
		le.PutUint16(out[2:], bigObjSig2)
		le.PutUint16(out[4:], bigObjVersion2)
		le.PutUint16(out[6:], IMAGE_FILE_MACHINE_AMD64)
		copy(out[12:28], bigClassIDForTest[:])
		le.PutUint32(out[44:], n)
		le.PutUint32(out[48:], symOff)
		le.PutUint32(out[52:], totalSlots)
	} else {
		le.PutUint16(out[0:], IMAGE_FILE_MACHINE_AMD64)
		le.PutUint16(out[2:], uint16(n))
		le.PutUint32(out[8:], symOff)
		le.PutUint32(out[12:], totalSlots)
	}

	// Section table.
	for i := range secs {
		sh := out[headerSize+uint32(i)*sectionHeader : headerSize+uint32(i+1)*sectionHeader]
		putSectionName(sh[0:8], secs[i].name)
		le.PutUint32(sh[16:], uint32(len(secs[i].data)))
		le.PutUint32(sh[20:], sectionData[i])
		le.PutUint32(sh[24:], sectionReloc[i])
		le.PutUint16(sh[32:], uint16(len(secs[i].relocs)))
		le.PutUint32(sh[36:], 0x60500020)
		if sectionData[i] != 0 {
			copy(out[sectionData[i]:], secs[i].data)
		}
		for j, r := range secs[i].relocs {
			rb := out[sectionReloc[i]+uint32(j)*relocRecord:]
			le.PutUint32(rb[0:], r.VirtualAddress)
			le.PutUint32(rb[4:], r.SymbolTableIndex)
			le.PutUint16(rb[8:], r.Type)
		}
	}

	// Symbol slots.
	for i, s := range syms {
		off := symOff + primarySlots[i]*stride
		rec := out[off : off+stride]
		putName(rec[0:8], s.name)
		le.PutUint32(rec[8:], s.value)
		if big {
			le.PutUint32(rec[12:], uint32(s.section))
			le.PutUint16(rec[16:], s.typ)
			rec[18] = s.class
			rec[19] = uint8(len(s.aux))
		} else {
			le.PutUint16(rec[12:], uint16(int16(s.section)))
			le.PutUint16(rec[14:], s.typ)
			rec[16] = s.class
			rec[17] = uint8(len(s.aux))
		}
		for j, a := range s.aux {
			auxOff := off + stride*uint32(1+j)
			copy(out[auxOff:auxOff+stride], a.raw)
		}
	}

	// String table: uint32 total length then NUL-terminated strings.
	le.PutUint32(out[stringTableOff:], stringTableLen)
	copy(out[stringTableOff+4:], stringBlob.Bytes())

	return &builtCOFF{
		raw:               out,
		big:               big,
		stride:            stride,
		symbolTableOffset: symOff,
		stringTableOffset: stringTableOff,
		primarySlots:      primarySlots,
		sectionData:       sectionData,
		sectionReloc:      sectionReloc,
	}
}

var bigClassIDForTest = [16]byte{
	0xc7, 0xa1, 0xba, 0xd1, 0xee, 0xba, 0xa9, 0x4b,
	0xaf, 0x20, 0xfa, 0xf6, 0x6a, 0xa4, 0xdc, 0xb8,
}

func itoa(v uint32) string {
	if v == 0 {
		return "0"
	}
	var buf [16]byte
	i := len(buf)
	for v > 0 {
		i--
		buf[i] = byte('0' + v%10)
		v /= 10
	}
	return string(buf[i:])
}

func weakAux(tag, characteristics uint32) testAux {
	raw := make([]byte, bigObjSymbolSize)
	binary.LittleEndian.PutUint32(raw[0:], tag)
	binary.LittleEndian.PutUint32(raw[4:], characteristics)
	return testAux{raw: raw}
}

// ---- TestTaskBigObj: >65535 sections, long names, multi-level weak chain ----

func TestTaskBigObj(t *testing.T) {
	taskcheck.Run(t, func(C *taskcheck.C) {
		const sectionCount = 65537 // genuinely beyond the 16-bit header field
		const longWeakName = "long_weak_external_symbol_name"
		const longSectionName = ".long_section_name_for_coff_test"

		secs := make([]testSection, sectionCount)
		secs[0] = testSection{
			name:   ".text",
			data:   []byte{0x90, 0x90, 0x90, 0xc3},
			relocs: []Reloc{{VirtualAddress: 7, SymbolTableIndex: 0, Type: IMAGE_REL_AMD64_ADDR64}},
		}
		secs[1] = testSection{name: longSectionName, data: []byte{1, 2, 3}}
		for i := 2; i < sectionCount-1; i++ {
			secs[i] = testSection{name: ".pad"}
		}
		bigData := []byte{0xaa, 0xbb, 0xcc, 0xdd, 0x11, 0x22, 0x33, 0x44}
		secs[sectionCount-1] = testSection{name: ".big", data: bigData}

		syms := []testSymbol{
			{name: "weak_one", section: 0, class: 105, aux: []testAux{weakAux(0, 1)}}, // tag fixed below
			{name: "filler", section: 1, class: 2, aux: []testAux{{raw: make([]byte, bigObjSymbolSize)}}},
			{name: longWeakName, section: 0, class: 105, aux: []testAux{weakAux(0, 2)}},
			{name: "weak_three", section: 0, class: 105, aux: []testAux{weakAux(0, 3)}},
			{name: "target_big", value: 4, section: int32(sectionCount), class: 2},
			{name: "abs_sym", section: -1, class: 3},
			{name: "dbg_sym", section: -2, class: 3},
		}
		built := buildTestCOFF(true, secs, syms)

		// Raw, aux-inclusive slot layout: 0 weak_one, 1 aux, 2 filler,
		// 3 aux, 4 long weak, 5 aux, 6 weak_three, 7 aux, 8 target...
		C.Check(built.primarySlots[0] == 0 && built.primarySlots[1] == 2 &&
			built.primarySlots[2] == 4 && built.primarySlots[3] == 6 &&
			built.primarySlots[4] == 8, "raw slot indexes include auxiliary slots: %v", built.primarySlots)
		// Patch TagIndex values now that stable slot indexes are known.
		le := binary.LittleEndian
		le.PutUint32(built.raw[built.symbolTableOffset+1*built.stride:], built.primarySlots[2])
		le.PutUint32(built.raw[built.symbolTableOffset+5*built.stride:], built.primarySlots[3])
		le.PutUint32(built.raw[built.symbolTableOffset+7*built.stride:], built.primarySlots[4])

		f, err := NewFile(bytes.NewReader(built.raw))
		C.Check(err == nil, "NewFile reads BigObj v2: %v", err)
		C.Check(f != nil && f.IsBigObj(), "file reports BigObj identity")
		C.Check(f.SectionCount() == sectionCount, "SectionCount keeps full 32-bit value: %d", f.SectionCount())
		C.Check(f.FileHeader.NumberOfSections == 0, "standard 16-bit header field is not a truncated count: %d", f.FileHeader.NumberOfSections)
		C.Check(f.BigObjHeader != nil && f.BigObjHeader.Version == 2 &&
			f.BigObjHeader.NumberOfSections == sectionCount, "BigObjHeader retains version and real section count")
		C.Check(f.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64, "machine decoded: %#x", f.FileHeader.Machine)

		C.Check(len(f.Sections) == sectionCount, "all section headers decoded: %d", len(f.Sections))
		textData, err := f.Sections[0].Data()
		C.Check(err == nil && bytes.Equal(textData, []byte{0x90, 0x90, 0x90, 0xc3}), "Section.Data via existing entry point: %x err=%v", textData, err)
		C.Check(f.Sections[1].Name == longSectionName, "long section name resolved: %q", f.Sections[1].Name)
		C.Check(len(f.Sections[sectionCount-1].Relocs) == 0, "pad sections stay usable")
		bigSecData, err := f.Sections[sectionCount-1].Data()
		C.Check(err == nil && bytes.Equal(bigSecData, bigData), "terminal section raw data: %x", bigSecData)

		C.Check(len(f.COFFSymbols) == 11, "COFFSymbols keeps every raw slot including aux: %d", len(f.COFFSymbols))
		C.Check(len(f.Symbols) == 7, "Symbols drops auxiliary records: %d", len(f.Symbols))
		C.Check(f.COFFSymbols[8].SectionNumber == int32(sectionCount), "defined section number >65535 not truncated: %d", f.COFFSymbols[8].SectionNumber)
		C.Check(f.Symbols[4].SectionNumber == int32(sectionCount), "Symbols carry widened section number: %d", f.Symbols[4].SectionNumber)
		C.Check(f.COFFSymbols[9].SectionNumber == -1 && f.COFFSymbols[10].SectionNumber == -2,
			"special section numbers -1/-2 preserved: %d %d", f.COFFSymbols[9].SectionNumber, f.COFFSymbols[10].SectionNumber)
		name4, err := f.COFFSymbols[4].FullName(f.StringTable)
		C.Check(err == nil && name4 == longWeakName, "long symbol name via 20-byte-stride string table: %q err=%v", name4, err)
		C.Check(len(f.Sections[0].Relocs) == 1 && f.Sections[0].Relocs[0].SymbolTableIndex == 0 &&
			f.Sections[0].Relocs[0].Type == IMAGE_REL_AMD64_ADDR64, "relocation decoded unchanged")

		ref, err := f.COFFReference(1, 0)
		C.Check(err == nil, "COFFReference succeeds: %v", err)
		C.Check(ref != nil, "reference owns its own value")
		C.Check(ref.RelocationOffset == built.sectionReloc[0], "relocation file position: got %#x want %#x", ref.RelocationOffset, built.sectionReloc[0])
		C.Check(ref.Relocation.VirtualAddress == 7 && ref.Relocation.Type == IMAGE_REL_AMD64_ADDR64 &&
			ref.Relocation.SymbolTableIndex == 0, "original relocation type and offset preserved")
		C.Check(len(ref.Chain) == 4, "chain visits four primary slots: %d", len(ref.Chain))
		wantSlots := []uint32{0, 4, 6, 8}
		wantNames := []string{"weak_one", longWeakName, "weak_three", "target_big"}
		wantChars := []uint32{1, 2, 3}
		for i, slot := range wantSlots {
			hop := ref.Chain[i]
			C.Check(hop.SlotIndex == slot, "hop %d raw slot index: got %d want %d", i, hop.SlotIndex, slot)
			C.Check(hop.Offset == built.symbolTableOffset+slot*built.stride,
				"hop %d file position: got %#x want %#x", i, hop.Offset, built.symbolTableOffset+slot*built.stride)
			C.Check(hop.Name == wantNames[i], "hop %d name: %q", i, hop.Name)
			C.Check(hop.StorageClass == f.COFFSymbols[slot].StorageClass, "hop %d storage class", i)
			C.Check(hop.SectionNumber == f.COFFSymbols[slot].SectionNumber, "hop %d full section number", i)
		}
		for i := 0; i < 3; i++ {
			w := ref.Chain[i].Weak
			wantAux := wantSlots[i] + 1
			C.Check(w != nil, "weak hop %d has auxiliary explanation", i)
			if w != nil {
				C.Check(w.AuxiliaryIndex == wantAux, "weak %d aux slot index: %d", i, w.AuxiliaryIndex)
				C.Check(w.AuxiliaryOffset == built.symbolTableOffset+wantAux*built.stride, "weak %d aux file position", i)
				C.Check(w.TagIndex == wantSlots[i+1], "weak %d TagIndex: %d", i, w.TagIndex)
				C.Check(w.Characteristics == wantChars[i], "weak %d raw characteristic preserved: %d", i, w.Characteristics)
				C.Check(len(w.SearchName) > 0, "weak %d characteristic named %q", i, w.SearchName)
			}
		}
		C.Check(ref.Chain[3].Weak == nil, "terminal slot is not weak")
		C.Check(ref.Terminal.Kind == COFFTerminalDefined, "terminal classified as defined: %d", ref.Terminal.Kind)
		C.Check(ref.Terminal.SectionNumber == int32(sectionCount) && ref.Terminal.SectionName == ".big", "terminal names big section")
		C.Check(ref.Terminal.Value == 4, "terminal keeps in-section offset")
		C.Check(ref.Terminal.HasDataOffset && ref.Terminal.DataOffset == built.sectionData[sectionCount-1]+4,
			"terminal raw byte file position: got %#x want %#x", ref.Terminal.DataOffset, built.sectionData[sectionCount-1]+4)

		// Repeating a query returns an independent, identical chain.
		absRef, err := f.COFFReference(1, 0)
		C.Check(err == nil && absRef != nil, "re-query after successful query works")
		C.Check(absRef.Terminal.Kind == COFFTerminalDefined && chainSlots(absRef)[3] == 8, "queries are independent and repeatable")
	})
}

// ---- TestTaskWeakSlots: weak chains, sharing, special sections, errors ----

func TestTaskWeakSlots(t *testing.T) {
	taskcheck.Run(t, func(C *taskcheck.C) {
		syms := []testSymbol{
			{name: "dup", section: 0, class: 105, aux: []testAux{weakAux(3, 1)}},                            // 0, aux 1
			{name: "dup", value: 1, section: 1, class: 2},                                                   // 2
			{name: "mid", section: 0, class: 105, aux: []testAux{weakAux(9, 2)}},                            // 3, aux 4
			{name: "wa", section: 0, class: 105, aux: []testAux{weakAux(3, 1)}},                             // 5, aux 6
			{name: "wb", section: 0, class: 105, aux: []testAux{weakAux(3, 3)}},                             // 7, aux 8
			{name: "shared_tail", value: 2, section: 1, class: 2},                                           // 9
			{name: "undef", section: 0, value: 0, class: 2},                                                 // 10
			{name: "common", section: 0, value: 16, class: 2},                                               // 11
			{name: "abs", section: -1, class: 3},                                                            // 12
			{name: "dbg", section: -2, class: 3},                                                            // 13
			{name: "outofrange", value: 999, section: 1, class: 2},                                          // 14
			{name: "loopx", section: 0, class: 105, aux: []testAux{weakAux(17, 1)}},                         // 15, aux 16
			{name: "loopy", section: 0, class: 105, aux: []testAux{weakAux(15, 2)}},                         // 17, aux 18
			{name: "badchar", section: 0, class: 105, aux: []testAux{weakAux(9, 99)}},                       // 19, aux 20
			{name: "noaux", section: 0, class: 105},                                                         // 21
			{name: "auxowner", section: 1, class: 2, aux: []testAux{{raw: make([]byte, bigObjSymbolSize)}}}, // 22, aux 23
		}
		relocs := []Reloc{
			{VirtualAddress: 0, SymbolTableIndex: 0, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 8, SymbolTableIndex: 2, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 16, SymbolTableIndex: 5, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 24, SymbolTableIndex: 7, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 32, SymbolTableIndex: 10, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 40, SymbolTableIndex: 11, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 48, SymbolTableIndex: 12, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 56, SymbolTableIndex: 13, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 64, SymbolTableIndex: 14, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 72, SymbolTableIndex: 23, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 80, SymbolTableIndex: 999, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 88, SymbolTableIndex: 15, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 96, SymbolTableIndex: 19, Type: IMAGE_REL_AMD64_ADDR64},
			{VirtualAddress: 104, SymbolTableIndex: 21, Type: IMAGE_REL_AMD64_ADDR64},
		}
		secs := []testSection{{name: ".text", data: []byte{0x90, 0x90, 0x90, 0xc3}, relocs: relocs}}
		built := buildTestCOFF(true, secs, syms)
		digestBefore := sha256.Sum256(built.raw)

		f, err := NewFile(bytes.NewReader(built.raw))
		C.Check(err == nil, "open weak-chain object: %v", err)

		ref0, err := f.COFFReference(1, 0)
		C.Check(err == nil, "weak chain query: %v", err)
		C.Check(len(ref0.Chain) == 3, "three hops through two weak symbols: %d", len(ref0.Chain))
		C.Check(ref0.Chain[0].SlotIndex == 0 && ref0.Chain[1].SlotIndex == 3 && ref0.Chain[2].SlotIndex == 9,
			"raw slot chain 0->3->9: %v", chainSlots(ref0))
		C.Check(ref0.Chain[0].Name == "dup" && ref0.Chain[2].Name == "shared_tail", "chain names: %v", chainNames(ref0))

		// Same name, distinct raw slot identity: never merged.
		ref1, err := f.COFFReference(1, 1)
		C.Check(err == nil, "same-name defined symbol query: %v", err)
		C.Check(len(ref1.Chain) == 1 && ref1.Chain[0].SlotIndex == 2, "defined dup keeps its own slot: %v", chainSlots(ref1))
		C.Check(ref1.Chain[0].Name == "dup" && ref1.Chain[0].SlotIndex != ref0.Chain[0].SlotIndex,
			"equal names do not merge slot identities")
		C.Check(ref1.Terminal.Kind == COFFTerminalDefined && ref1.Terminal.Value == 1, "defined dup terminal")

		// Two references share one intermediate symbol and one tail slot.
		ref2, err := f.COFFReference(1, 2)
		C.Check(err == nil, "wa query: %v", err)
		ref3, err := f.COFFReference(1, 3)
		C.Check(err == nil, "wb query: %v", err)
		C.Check(chainSlots(ref2)[1] == 3 && chainSlots(ref3)[1] == 3, "intermediate slot shared")
		C.Check(ref2.Chain[len(ref2.Chain)-1].SlotIndex == 9 &&
			ref3.Chain[len(ref3.Chain)-1].SlotIndex == 9, "tail slot shared")
		C.Check(ref2.Chain[0].Weak.Characteristics == 1 && ref3.Chain[0].Weak.Characteristics == 3,
			"search characteristics 1 and 3 kept distinct: %d %d", ref2.Chain[0].Weak.Characteristics, ref3.Chain[0].Weak.Characteristics)

		// Terminal classifications, including zero-fill/out-of-raw-range.
		kinds := []COFFTerminalKind{COFFTerminalUndefined, COFFTerminalCommon, COFFTerminalAbsolute, COFFTerminalDebug, COFFTerminalDefined}
		for i, want := range kinds {
			r, qerr := f.COFFReference(1, uint32(4+i))
			C.Check(qerr == nil, "terminal query %d: %v", i, qerr)
			C.Check(r.Terminal.Kind == want, "terminal %d kind: got %d want %d", i, r.Terminal.Kind, want)
		}
		common, _ := f.COFFReference(1, 5)
		C.Check(common.Terminal.Value == 16, "common keeps declared size")
		outOfRange, _ := f.COFFReference(1, 8)
		C.Check(!outOfRange.Terminal.HasDataOffset, "offset beyond raw data reports no invented file location")
		defined, _ := f.COFFReference(1, 1)
		C.Check(defined.Terminal.HasDataOffset && defined.Terminal.DataOffset == built.sectionData[0]+1,
			"in-range offset names a real raw byte")

		// Error cases: every slot-related failure carries the raw position.
		expectSlotError := func(relocIndex, wantSlot uint32, label string) {
			_, e := f.COFFReference(1, relocIndex)
			C.Check(e != nil, "%s returns an error", label)
			var re *COFFReferenceError
			C.Check(errors.As(e, &re), "%s error is *COFFReferenceError: %T", label, e)
			if re != nil {
				C.Check(re.SlotIndex == wantSlot, "%s names slot %d: got %d", label, wantSlot, re.SlotIndex)
				C.Check(re.SlotOffset == built.symbolTableOffset+wantSlot*built.stride,
					"%s carries slot file position", label)
			}
		}
		expectSlotError(9, 23, "relocation targeting auxiliary slot")
		expectSlotError(10, 999, "symbol index beyond table")
		expectSlotError(11, 15, "weak fallback loop")
		expectSlotError(12, 20, "unknown search characteristic")
		expectSlotError(13, 21, "weak symbol missing auxiliary")

		_, err = f.COFFReference(0, 0)
		C.Check(err != nil, "zero section index rejected")
		_, err = f.COFFReference(99, 0)
		C.Check(err != nil, "section index beyond table rejected")
		_, err = f.COFFReference(1, 99)
		C.Check(err != nil, "relocation index beyond section rejected")

		// A failed query must not poison later, normal queries.
		again, err := f.COFFReference(1, 0)
		C.Check(err == nil && again != nil && chainSlots(again)[0] == 0, "normal query works after failures")

		// Returned results own their data: mutating them leaves File alone.
		ref0.Chain[0].Name = "locally_mutated"
		ref0.Chain = append(ref0.Chain, COFFSymbolRef{})
		fresh, err := f.COFFReference(1, 0)
		C.Check(err == nil && fresh.Chain[0].Name == "dup" && len(fresh.Chain) == 3,
			"result does not borrow File slices")
		name0, _ := f.COFFSymbols[0].FullName(f.StringTable)
		C.Check(name0 == "dup", "File symbol table untouched by result mutation")

		// Reads and queries never modify the input bytes.
		C.Check(sha256.Sum256(built.raw) == digestBefore, "input bytes unchanged after read and queries")
	})
}

// ---- TestTaskCompatibility: ordinary COFF, PE32/PE32+, short import, write ----

func TestTaskCompatibility(t *testing.T) {
	taskcheck.Run(t, func(C *taskcheck.C) {
		// Ordinary COFF object through the unchanged entry points.
		ordinary := fixture.Basic(false)
		f, err := NewFile(bytes.NewReader(ordinary))
		C.Check(err == nil, "ordinary COFF opens: %v", err)
		C.Check(!f.IsBigObj() && f.SectionCount() == 1, "ordinary identity and count")
		C.Check(f.FileHeader.NumberOfSections == 1, "standard FileHeader still describes the disk header")
		C.Check(len(f.Sections) == 1 && len(f.Symbols) == 1 && len(f.COFFSymbols) == 1, "ordinary counts")
		C.Check(f.Symbols[0].Name == "anchor" && f.Symbols[0].SectionNumber == 1, "ordinary symbol values")
		data, err := f.Sections[0].Data()
		C.Check(err == nil && bytes.Equal(data, []byte{0x90, 0x90, 0x90, 0xc3}), "ordinary section data")
		oref, err := f.COFFReference(1, 0)
		C.Check(err == nil, "ordinary COFFReference: %v", err)
		C.Check(oref.RelocationOffset == 64 && oref.Chain[0].SlotIndex == 0 && oref.Chain[0].Offset == 74,
			"18-byte stride positions: reloc=%#x slot=%#x", oref.RelocationOffset, oref.Chain[0].Offset)
		C.Check(oref.Terminal.Kind == COFFTerminalDefined && oref.Terminal.HasDataOffset, "ordinary terminal")

		// A weak external in an ordinary 18-byte object reads with stride 18.
		weakOrd := buildTestCOFF(false,
			[]testSection{{name: ".text", data: []byte{0xc3}, relocs: []Reloc{{SymbolTableIndex: 0, Type: IMAGE_REL_AMD64_ADDR64}}}},
			[]testSymbol{
				{name: "long_ordinary_weak_name", section: 0, class: 105, aux: []testAux{weakAuxOrdinary(2, 2)}},
				{name: "ord_tail", value: 0, section: 1, class: 2},
			})
		wf, err := NewFile(bytes.NewReader(weakOrd.raw))
		C.Check(err == nil, "ordinary weak object opens: %v", err)
		wref, err := wf.COFFReference(1, 0)
		C.Check(err == nil, "ordinary weak reference: %v", err)
		C.Check(len(wref.Chain) == 2 && wref.Chain[0].Weak.TagIndex == 2, "ordinary weak chain: %v", chainSlots(wref))
		C.Check(wref.Chain[0].Weak.AuxiliaryOffset == weakOrd.symbolTableOffset+18,
			"auxiliary read at 18-byte stride: %#x", wref.Chain[0].Weak.AuxiliaryOffset)
		longName, err := wf.COFFSymbols[0].FullName(wf.StringTable)
		C.Check(err == nil && longName == "long_ordinary_weak_name", "string table located via 18-byte stride: %q", longName)

		// The 00 00 ff ff signature alone must not imply BigObj: short
		// import objects (version 0x100 here) are rejected explicitly.
		shortImport := make([]byte, 20)
		binary.LittleEndian.PutUint16(shortImport[0:], 0)
		binary.LittleEndian.PutUint16(shortImport[2:], 0xffff)
		binary.LittleEndian.PutUint16(shortImport[4:], 0x0100)
		binary.LittleEndian.PutUint16(shortImport[6:], IMAGE_FILE_MACHINE_AMD64)
		_, err = NewFile(bytes.NewReader(shortImport))
		C.Check(err != nil, "short import object distinguished from BigObj: %v", err)

		// PE32 and PE32+ images still read, with their optional headers.
		pe32File, openErr := os.Open(filepath.Join("testdata", "gcc-386-mingw-exec"))
		C.Check(openErr == nil, "open PE32 testdata: %v", openErr)
		if openErr == nil {
			pe32, perr := NewFile(pe32File)
			C.Check(perr == nil, "PE32 opens: %v", perr)
			_, ok := pe32.OptionalHeader.(*OptionalHeader32)
			C.Check(perr == nil && ok, "PE32 optional header type")
			C.Check(perr == nil && !pe32.IsBigObj() && pe32.SectionCount() == uint32(pe32.FileHeader.NumberOfSections), "PE32 section count")
			pe32File.Close()
		}
		pe64File, openErr := os.Open(filepath.Join("testdata", "gcc-amd64-mingw-exec"))
		C.Check(openErr == nil, "open PE32+ testdata: %v", openErr)
		if openErr == nil {
			pe64, perr := NewFile(pe64File)
			_, ok := pe64.OptionalHeader.(*OptionalHeader64)
			C.Check(perr == nil && ok, "PE32+ optional header type")
			pe64File.Close()
		}

		// Ordinary PE remains writable using 18-byte symbol records.
		src := filepath.Join("testdata", "gcc-386-mingw-exec")
		orig, err := Open(src)
		C.Check(err == nil, "open PE for write roundtrip: %v", err)
		raw, err := orig.Bytes()
		C.Check(err == nil, "ordinary PE Bytes: %v", err)
		reopened, err := NewFile(bytes.NewReader(raw))
		C.Check(err == nil, "written PE reopens: %v", err)
		C.Check(len(reopened.COFFSymbols) == len(orig.COFFSymbols), "all 18-byte symbol records round-tripped")
		C.Check(symbolSectionEqual(orig.COFFSymbols, reopened.COFFSymbols), "widened in-memory fields serialize back at 18 bytes")
		tmp := filepath.Join(t.TempDir(), "roundtrip.exe")
		C.Check(orig.WriteFile(tmp) == nil, "WriteFile on ordinary PE")
		fromDisk, err := Open(tmp)
		C.Check(err == nil && fromDisk != nil, "written file reopens from disk: %v", err)
		C.Check(fromDisk.SectionCount() == orig.SectionCount(), "section count survives WriteFile")
		orig.Close()

		// BigObj is read-only: writing is explicitly unsupported.
		big, err := NewFile(bytes.NewReader(fixture.Basic(true)))
		C.Check(err == nil, "BigObj fixture opens: %v", err)
		C.Check(big.IsBigObj() && big.SectionCount() == 1, "BigObj fixture identity")
		_, err = big.Bytes()
		C.Check(err != nil, "BigObj Bytes returns unsupported: %v", err)
		C.Check(big.WriteFile(filepath.Join(t.TempDir(), "should-not-exist.obj")) != nil, "BigObj WriteFile returns unsupported")
	})
}

func weakAuxOrdinary(tag, characteristics uint32) testAux {
	raw := make([]byte, COFFSymbolSize)
	binary.LittleEndian.PutUint32(raw[0:], tag)
	binary.LittleEndian.PutUint32(raw[4:], characteristics)
	return testAux{raw: raw}
}

func chainSlots(r *COFFReference) []uint32 {
	out := make([]uint32, len(r.Chain))
	for i := range r.Chain {
		out[i] = r.Chain[i].SlotIndex
	}
	return out
}

func chainNames(r *COFFReference) []string {
	out := make([]string, len(r.Chain))
	for i := range r.Chain {
		out[i] = r.Chain[i].Name
	}
	return out
}

func symbolSectionEqual(a, b []COFFSymbol) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
