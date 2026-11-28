package pe

import (
	"bytes"
	"encoding/binary"
	"errors"
	"path"
	"path/filepath"
	"strconv"
	"strings"
	"testing"

	"github.com/Binject/debug/task_support/fixture"
	"github.com/Binject/debug/task_support/taskcheck"
)

// taskSection describes one section of a synthetic COFF object.
type taskSection struct {
	name            string
	data            []byte
	relocs          []Reloc
	characteristics uint32
}

// taskSymbol describes one primary symbol of a synthetic COFF object.
// Each entry of aux is one raw auxiliary record; short records are padded
// to the symbol stride with zeros.
type taskSymbol struct {
	name  string
	value uint32
	sect  int32
	class uint8
	aux   [][]byte
}

// taskWeakAux builds a weak external auxiliary record (TagIndex,
// Characteristics, rest unused).
func taskWeakAux(tagIndex, characteristics uint32) []byte {
	rec := make([]byte, COFFSymbolBigObjSize)
	binary.LittleEndian.PutUint32(rec[0:], tagIndex)
	binary.LittleEndian.PutUint32(rec[4:], characteristics)
	return rec
}

// buildTaskObject constructs a small but legal COFF object, either an
// ordinary 20-byte-header object with 18-byte symbol records or an AMD64
// BigObj v2 object with 20-byte symbol records.
func buildTaskObject(big bool, sections []taskSection, syms []taskSymbol) []byte {
	header, stride := 20, COFFSymbolSize
	if big {
		header, stride = bigObjHeaderSize, COFFSymbolBigObjSize
	}

	// String table: long section and symbol names, slash-decimal for
	// sections, zero-plus-offset for symbols.
	strtab := []byte{0, 0, 0, 0}
	strOff := func(s string) uint32 {
		off := uint32(len(strtab))
		strtab = append(strtab, s...)
		strtab = append(strtab, 0)
		return off
	}
	secNameOff := make([]uint32, len(sections))
	secLong := make([]bool, len(sections))
	for i, s := range sections {
		if len(s.name) > 8 {
			secLong[i] = true
			secNameOff[i] = strOff(s.name)
		}
	}
	symNameOff := make([]uint32, len(syms))
	symLong := make([]bool, len(syms))
	for i, s := range syms {
		if len(s.name) > 8 {
			symLong[i] = true
			symNameOff[i] = strOff(s.name)
		}
	}

	// Layout: header, section headers, section data, relocations, symbol
	// table, string table.
	off := header + 40*len(sections)
	secDataOff := make([]uint32, len(sections))
	secRelocOff := make([]uint32, len(sections))
	for i, s := range sections {
		if len(s.data) > 0 {
			secDataOff[i] = uint32(off)
			off += len(s.data)
		}
		if len(s.relocs) > 0 {
			secRelocOff[i] = uint32(off)
			off += coffRelocRecordSize * len(s.relocs)
		}
	}
	symOff := off
	numSyms := 0
	for _, s := range syms {
		numSyms += 1 + len(s.aux)
	}
	off += numSyms * stride
	binary.LittleEndian.PutUint32(strtab, uint32(len(strtab)))

	b := make([]byte, off+len(strtab))
	if big {
		binary.LittleEndian.PutUint16(b[2:], 0xffff)
		binary.LittleEndian.PutUint16(b[4:], 2)
		binary.LittleEndian.PutUint16(b[6:], IMAGE_FILE_MACHINE_AMD64)
		copy(b[12:28], bigObjClassID[:])
		binary.LittleEndian.PutUint32(b[44:], uint32(len(sections)))
		binary.LittleEndian.PutUint32(b[48:], uint32(symOff))
		binary.LittleEndian.PutUint32(b[52:], uint32(numSyms))
	} else {
		binary.LittleEndian.PutUint16(b[0:], IMAGE_FILE_MACHINE_AMD64)
		binary.LittleEndian.PutUint16(b[2:], uint16(len(sections)))
		binary.LittleEndian.PutUint32(b[8:], uint32(symOff))
		binary.LittleEndian.PutUint32(b[12:], uint32(numSyms))
	}

	for i, s := range sections {
		sh := b[header+40*i:]
		if secLong[i] {
			sh[0] = '/'
			copy(sh[1:8], []byte(strconv.Itoa(int(secNameOff[i]))))
		} else {
			copy(sh[0:8], s.name)
		}
		binary.LittleEndian.PutUint32(sh[16:], uint32(len(s.data)))
		binary.LittleEndian.PutUint32(sh[20:], secDataOff[i])
		binary.LittleEndian.PutUint32(sh[24:], secRelocOff[i])
		binary.LittleEndian.PutUint16(sh[32:], uint16(len(s.relocs)))
		binary.LittleEndian.PutUint32(sh[36:], s.characteristics)
		copy(b[secDataOff[i]:], s.data)
		for j, rel := range s.relocs {
			ro := secRelocOff[i] + uint32(coffRelocRecordSize*j)
			binary.LittleEndian.PutUint32(b[ro:], rel.VirtualAddress)
			binary.LittleEndian.PutUint32(b[ro+4:], rel.SymbolTableIndex)
			binary.LittleEndian.PutUint16(b[ro+8:], rel.Type)
		}
	}

	so := symOff
	for i, s := range syms {
		rec := b[so:]
		if symLong[i] {
			binary.LittleEndian.PutUint32(rec[4:], symNameOff[i])
		} else {
			copy(rec[0:8], s.name)
		}
		binary.LittleEndian.PutUint32(rec[8:], s.value)
		if big {
			binary.LittleEndian.PutUint32(rec[12:], uint32(s.sect))
			rec[18] = s.class
			rec[19] = uint8(len(s.aux))
		} else {
			binary.LittleEndian.PutUint16(rec[12:], uint16(int16(s.sect)))
			rec[16] = s.class
			rec[17] = uint8(len(s.aux))
		}
		so += stride
		for _, a := range s.aux {
			copy(b[so:], a)
			so += stride
		}
	}
	copy(b[so:], strtab)
	return b
}

// TestTaskBigObj reads a BigObj v2 object with more than 65535 sections and
// resolves a relocation to a definition in a high-numbered section.
func TestTaskBigObj(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		const numSections = 65540
		const highSect = 65538 // 1-based section number of the definition
		const longName = ".very_long_section_name_for_target"

		sections := make([]taskSection, numSections)
		sections[0] = taskSection{
			name: ".text",
			data: []byte{0x90, 0x90, 0x90, 0xc3},
			relocs: []Reloc{
				{VirtualAddress: 0, SymbolTableIndex: 0, Type: 0x0004},
			},
			characteristics: 0x60500020,
		}
		sections[highSect-1] = taskSection{
			name:            longName,
			data:            []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88},
			characteristics: 0x40300040,
		}
		syms := []taskSymbol{
			{name: "impl_in_high_section", value: 2, sect: highSect, class: 2},
		}
		raw := buildTaskObject(true, sections, syms)

		f, err := NewFile(bytes.NewReader(raw))
		c.Check(err == nil, "NewFile bigobj: %v", err)
		if err != nil {
			return
		}
		c.Check(f.IsBigObj(), "file should be recognized as BigObj")
		c.Check(f.SectionCount() == numSections, "SectionCount = %d, want %d", f.SectionCount(), numSections)
		c.Check(len(f.Sections) == numSections, "len(Sections) = %d, want %d", len(f.Sections), numSections)
		c.Check(f.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64, "machine = 0x%x", f.FileHeader.Machine)
		c.Check(f.BigObjHeader != nil && f.BigObjHeader.NumberOfSections == numSections,
			"BigObjHeader.NumberOfSections = %d", f.BigObjHeader.NumberOfSections)

		high := f.Sections[highSect-1]
		c.Check(high.Name == longName, "high section name = %q, want %q", high.Name, longName)
		data, derr := high.Data()
		c.Check(derr == nil && bytes.Equal(data, []byte{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}),
			"high section data = %x, err = %v", data, derr)

		c.Check(len(f.Sections[0].Relocs) == 1, "relocs = %d, want 1", len(f.Sections[0].Relocs))
		c.Check(len(f.COFFSymbols) == 1, "COFFSymbols = %d, want 1", len(f.COFFSymbols))
		c.Check(f.COFFSymbols[0].SectionNumber == highSect,
			"COFFSymbol.SectionNumber = %d, want %d (untruncated)", f.COFFSymbols[0].SectionNumber, highSect)
		c.Check(len(f.Symbols) == 1, "Symbols = %d, want 1", len(f.Symbols))
		if len(f.Symbols) == 1 {
			c.Check(f.Symbols[0].Name == "impl_in_high_section", "symbol name = %q", f.Symbols[0].Name)
			c.Check(f.Symbols[0].SectionNumber == highSect,
				"Symbol.SectionNumber = %d, want %d (untruncated)", f.Symbols[0].SectionNumber, highSect)
			c.Check(f.Symbols[0].StorageClass == 2, "storage class = %d", f.Symbols[0].StorageClass)
		}

		ref, err := f.COFFReference(1, 0)
		c.Check(err == nil, "COFFReference: %v", err)
		if err != nil {
			return
		}
		c.Check(ref.SectionIndex == 1 && ref.SectionName == ".text",
			"source section = %d %q", ref.SectionIndex, ref.SectionName)
		c.Check(ref.RelocationIndex == 0, "relocation index = %d", ref.RelocationIndex)
		c.Check(ref.Reloc.Type == 0x0004 && ref.Reloc.VirtualAddress == 0 && ref.Reloc.SymbolTableIndex == 0,
			"reloc = %+v", ref.Reloc)
		c.Check(ref.RelocationOffset == f.Sections[0].PointerToRelocations,
			"relocation file offset = %d, want %d", ref.RelocationOffset, f.Sections[0].PointerToRelocations)
		c.Check(binary.LittleEndian.Uint16(raw[ref.RelocationOffset+8:]) == 0x0004,
			"raw relocation type at file offset = 0x%x", binary.LittleEndian.Uint16(raw[ref.RelocationOffset+8:]))

		c.Check(len(ref.Chain) == 1, "chain length = %d, want 1", len(ref.Chain))
		if len(ref.Chain) == 1 {
			hop := ref.Chain[0]
			c.Check(hop.SlotIndex == 0, "hop slot = %d", hop.SlotIndex)
			c.Check(hop.Offset == f.FileHeader.PointerToSymbolTable,
				"hop file offset = %d, want %d", hop.Offset, f.FileHeader.PointerToSymbolTable)
			c.Check(binary.LittleEndian.Uint32(raw[hop.Offset+12:]) == highSect,
				"raw section number at slot = %d", binary.LittleEndian.Uint32(raw[hop.Offset+12:]))
			c.Check(hop.Name == "impl_in_high_section", "hop name = %q", hop.Name)
			c.Check(hop.Value == 2 && hop.StorageClass == 2 && hop.SectionNumber == highSect,
				"hop value=%d class=%d sect=%d", hop.Value, hop.StorageClass, hop.SectionNumber)
			c.Check(hop.Weak == nil, "definition hop should not be weak")
		}

		term := ref.Terminal
		c.Check(term.Kind == COFFTerminalDefined, "terminal kind = %v", term.Kind)
		c.Check(term.SectionIndex == highSect && term.SectionName == longName,
			"terminal section = %d %q", term.SectionIndex, term.SectionName)
		c.Check(term.SectionOffset == 2, "terminal section offset = %d", term.SectionOffset)
		c.Check(term.HasFileOffset, "terminal should have a file offset")
		c.Check(term.FileOffset == high.Offset+2, "terminal file offset = %d, want %d", term.FileOffset, high.Offset+2)
		c.Check(raw[term.FileOffset] == 0x33, "byte at terminal file offset = 0x%x, want 0x33", raw[term.FileOffset])
	})
}

// TestTaskWeakSlots resolves multi-level weak external fallback chains in a
// BigObj with interleaved auxiliary slots, same-name symbols, shared chain
// tails and special section numbers, and exercises the error paths.
func TestTaskWeakSlots(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		sections := []taskSection{
			{
				name: ".text",
				data: []byte{0xcc, 0xcc, 0xcc, 0xcc},
				relocs: []Reloc{
					{VirtualAddress: 0, SymbolTableIndex: 0, Type: 0x0004},    // weak chain to final_impl
					{VirtualAddress: 1, SymbolTableIndex: 7, Type: 0x0004},    // dup_name -> final_impl (shared tail)
					{VirtualAddress: 2, SymbolTableIndex: 9, Type: 0x0004},    // dup_name -> other_impl (same name)
					{VirtualAddress: 3, SymbolTableIndex: 12, Type: 0x0004},   // undefined
					{VirtualAddress: 0, SymbolTableIndex: 13, Type: 0x0004},   // common
					{VirtualAddress: 0, SymbolTableIndex: 14, Type: 0x0004},   // absolute
					{VirtualAddress: 0, SymbolTableIndex: 15, Type: 0x0004},   // debug
					{VirtualAddress: 0, SymbolTableIndex: 1, Type: 0x0004},    // points at auxiliary slot
					{VirtualAddress: 0, SymbolTableIndex: 16, Type: 0x0004},   // weak without auxiliary record
					{VirtualAddress: 0, SymbolTableIndex: 17, Type: 0x0004},   // unknown search characteristics
					{VirtualAddress: 0, SymbolTableIndex: 19, Type: 0x0004},   // fallback cycle
					{VirtualAddress: 0, SymbolTableIndex: 1000, Type: 0x0004}, // slot out of range
				},
				characteristics: 0x60500020,
			},
			{
				name:            ".data",
				data:            []byte{0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7},
				characteristics: 0xc0300040,
			},
		}
		syms := []taskSymbol{
			/* 0*/ {name: "weak_one", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(2, IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY)}},
			/* 2*/ {name: "weak_two", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(4, IMAGE_WEAK_EXTERN_SEARCH_LIBRARY)}},
			/* 4*/ {name: "weak_three", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(6, IMAGE_WEAK_EXTERN_SEARCH_ALIAS)}},
			/* 6*/ {name: "final_impl", value: 1, sect: 2, class: 2},
			/* 7*/ {name: "dup_name", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(6, IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY)}},
			/* 9*/ {name: "dup_name", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(11, IMAGE_WEAK_EXTERN_SEARCH_LIBRARY)}},
			/*11*/ {name: "other_impl", value: 3, sect: 2, class: 2},
			/*12*/ {name: "undef_sym", class: 2},
			/*13*/ {name: "common_sym", value: 16, class: 2},
			/*14*/ {name: "abs_sym", value: 0x1234, sect: -1, class: 2},
			/*15*/ {name: "debug_sym", sect: -2, class: 2},
			/*16*/ {name: "weak_noaux", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL},
			/*17*/ {name: "weak_badchar", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(6, 7)}},
			/*19*/ {name: "weak_loop_a", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(21, IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY)}},
			/*21*/ {name: "weak_loop_b", class: IMAGE_SYM_CLASS_WEAK_EXTERNAL, aux: [][]byte{taskWeakAux(19, IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY)}},
		}
		raw := buildTaskObject(true, sections, syms)

		f, err := NewFile(bytes.NewReader(raw))
		c.Check(err == nil, "NewFile bigobj: %v", err)
		if err != nil {
			return
		}
		c.Check(len(f.COFFSymbols) == 23, "raw slots = %d, want 23", len(f.COFFSymbols))
		c.Check(len(f.Symbols) == 15, "primary symbols = %d, want 15", len(f.Symbols))
		symTab := f.FileHeader.PointerToSymbolTable

		// Multi-level weak fallback: weak_one -> weak_two -> weak_three -> final_impl.
		ref, err := f.COFFReference(1, 0)
		c.Check(err == nil, "COFFReference weak chain: %v", err)
		if err != nil {
			return
		}
		c.Check(len(ref.Chain) == 4, "chain length = %d, want 4", len(ref.Chain))
		if len(ref.Chain) == 4 {
			wantSlots := []uint32{0, 2, 4, 6}
			wantNames := []string{"weak_one", "weak_two", "weak_three", "final_impl"}
			wantTags := []uint32{2, 4, 6}
			wantChars := []uint32{1, 2, 3}
			for i := range wantSlots {
				hop := ref.Chain[i]
				c.Check(hop.SlotIndex == wantSlots[i], "hop %d slot = %d, want %d", i, hop.SlotIndex, wantSlots[i])
				c.Check(hop.Name == wantNames[i], "hop %d name = %q, want %q", i, hop.Name, wantNames[i])
				c.Check(hop.Offset == symTab+wantSlots[i]*COFFSymbolBigObjSize,
					"hop %d file offset = %d, want %d", i, hop.Offset, symTab+wantSlots[i]*COFFSymbolBigObjSize)
				if i < 3 {
					c.Check(hop.StorageClass == IMAGE_SYM_CLASS_WEAK_EXTERNAL, "hop %d class = %d", i, hop.StorageClass)
					c.Check(hop.Weak != nil, "hop %d should be weak", i)
					if hop.Weak != nil {
						c.Check(hop.Weak.TagIndex == wantTags[i], "hop %d tag = %d, want %d", i, hop.Weak.TagIndex, wantTags[i])
						c.Check(hop.Weak.Characteristics == wantChars[i], "hop %d characteristics = %d, want %d", i, hop.Weak.Characteristics, wantChars[i])
						c.Check(hop.Weak.SlotIndex == wantSlots[i]+1, "hop %d aux slot = %d", i, hop.Weak.SlotIndex)
						c.Check(hop.Weak.Offset == hop.Offset+COFFSymbolBigObjSize, "hop %d aux offset = %d", i, hop.Weak.Offset)
						c.Check(binary.LittleEndian.Uint32(raw[hop.Weak.Offset:]) == wantTags[i],
							"hop %d raw tag at aux offset = %d", i, binary.LittleEndian.Uint32(raw[hop.Weak.Offset:]))
					}
				} else {
					c.Check(hop.Weak == nil, "terminal hop should not be weak")
					c.Check(hop.Value == 1 && hop.SectionNumber == 2, "terminal hop value=%d sect=%d", hop.Value, hop.SectionNumber)
				}
			}
		}
		c.Check(ref.Terminal.Kind == COFFTerminalDefined, "terminal kind = %v", ref.Terminal.Kind)
		c.Check(ref.Terminal.SectionIndex == 2 && ref.Terminal.SectionName == ".data",
			"terminal section = %d %q", ref.Terminal.SectionIndex, ref.Terminal.SectionName)
		c.Check(ref.Terminal.SectionOffset == 1 && ref.Terminal.HasFileOffset,
			"terminal offset = %d hasFileOffset = %v", ref.Terminal.SectionOffset, ref.Terminal.HasFileOffset)
		c.Check(raw[ref.Terminal.FileOffset] == 0xa1, "terminal byte = 0x%x, want 0xa1", raw[ref.Terminal.FileOffset])

		// Same-name weak symbols keep their own slot identity; the two
		// dup_name slots lead to different definitions, and the first one
		// shares its chain tail with the weak_one chain.
		refDup1, err1 := f.COFFReference(1, 1)
		refDup2, err2 := f.COFFReference(1, 2)
		c.Check(err1 == nil && err2 == nil, "dup_name references: %v / %v", err1, err2)
		if err1 == nil && err2 == nil {
			c.Check(len(refDup1.Chain) == 2 && len(refDup2.Chain) == 2,
				"dup chain lengths = %d, %d", len(refDup1.Chain), len(refDup2.Chain))
			if len(refDup1.Chain) == 2 && len(refDup2.Chain) == 2 && len(ref.Chain) == 4 {
				c.Check(refDup1.Chain[0].SlotIndex == 7 && refDup2.Chain[0].SlotIndex == 9,
					"dup first slots = %d, %d", refDup1.Chain[0].SlotIndex, refDup2.Chain[0].SlotIndex)
				c.Check(refDup1.Chain[0].Name == "dup_name" && refDup2.Chain[0].Name == "dup_name",
					"dup names = %q, %q", refDup1.Chain[0].Name, refDup2.Chain[0].Name)
				c.Check(refDup1.Chain[1].SlotIndex == 6 && refDup2.Chain[1].SlotIndex == 11,
					"dup tail slots = %d, %d", refDup1.Chain[1].SlotIndex, refDup2.Chain[1].SlotIndex)
				// Shared tail: slot 6 is the terminal of both ref and refDup1.
				c.Check(ref.Chain[3].SlotIndex == refDup1.Chain[1].SlotIndex,
					"shared tail slots = %d, %d", ref.Chain[3].SlotIndex, refDup1.Chain[1].SlotIndex)
				c.Check(ref.Terminal == refDup1.Terminal, "shared terminal = %+v vs %+v", ref.Terminal, refDup1.Terminal)
				c.Check(refDup2.Terminal.SectionOffset == 3 && raw[refDup2.Terminal.FileOffset] == 0xa3,
					"dup2 terminal offset = %d", refDup2.Terminal.SectionOffset)
			}
		}

		// Special section numbers: undefined, common, absolute, debug.
		refUndef, errU := f.COFFReference(1, 3)
		c.Check(errU == nil, "undefined reference: %v", errU)
		if errU == nil {
			c.Check(refUndef.Terminal.Kind == COFFTerminalUndefined,
				"undefined terminal = %v", refUndef.Terminal.Kind)
		}
		refCommon, errC := f.COFFReference(1, 4)
		c.Check(errC == nil, "common reference: %v", errC)
		if errC == nil {
			c.Check(refCommon.Terminal.Kind == COFFTerminalCommon && refCommon.Chain[0].Value == 16,
				"common terminal = %v value = %d", refCommon.Terminal.Kind, refCommon.Chain[0].Value)
		}
		refAbs, errA := f.COFFReference(1, 5)
		c.Check(errA == nil, "absolute reference: %v", errA)
		if errA == nil {
			c.Check(refAbs.Terminal.Kind == COFFTerminalAbsolute && refAbs.Chain[0].SectionNumber == -1,
				"absolute terminal = %v sect = %d", refAbs.Terminal.Kind, refAbs.Chain[0].SectionNumber)
		}
		refDbg, errD := f.COFFReference(1, 6)
		c.Check(errD == nil, "debug reference: %v", errD)
		if errD == nil {
			c.Check(refDbg.Terminal.Kind == COFFTerminalDebug && refDbg.Chain[0].SectionNumber == -2,
				"debug terminal = %v sect = %d", refDbg.Terminal.Kind, refDbg.Chain[0].SectionNumber)
		}

		// Error paths; slot-related errors must carry the raw slot position.
		slotErrCheck := func(sectionIndex, relocationIndex uint32, wantSlot uint32) {
			r, err := f.COFFReference(sectionIndex, relocationIndex)
			c.Check(err != nil, "COFFReference(%d, %d) should fail", sectionIndex, relocationIndex)
			c.Check(r == nil, "COFFReference(%d, %d) must not deliver a partial chain", sectionIndex, relocationIndex)
			var refErr *COFFReferenceError
			c.Check(errors.As(err, &refErr), "error type = %T", err)
			if errors.As(err, &refErr) {
				c.Check(refErr.HasSlot && refErr.SlotIndex == wantSlot,
					"error slot = %d (has %v), want %d", refErr.SlotIndex, refErr.HasSlot, wantSlot)
				c.Check(refErr.SlotOffset == symTab+wantSlot*COFFSymbolBigObjSize,
					"error slot offset = %d, want %d", refErr.SlotOffset, symTab+wantSlot*COFFSymbolBigObjSize)
			}
		}
		slotErrCheck(1, 7, 1)     // relocation points at an auxiliary slot
		slotErrCheck(1, 8, 16)    // weak external without auxiliary record
		slotErrCheck(1, 9, 17)    // unknown search characteristics
		slotErrCheck(1, 10, 19)   // fallback cycle
		slotErrCheck(1, 11, 1000) // slot index out of range

		// Out-of-range section and relocation indexes fail without slot info.
		for _, idx := range [][2]uint32{{0, 0}, {3, 0}, {1, 12}} {
			r, err := f.COFFReference(idx[0], idx[1])
			c.Check(err != nil && r == nil, "COFFReference(%d, %d) should fail", idx[0], idx[1])
			var refErr *COFFReferenceError
			if errors.As(err, &refErr) {
				c.Check(!refErr.HasSlot, "COFFReference(%d, %d) error should not carry a slot", idx[0], idx[1])
			} else {
				c.Check(false, "error type = %T", err)
			}
		}

		// After failed lookups a normal reference still resolves.
		refAgain, errAgain := f.COFFReference(1, 0)
		c.Check(errAgain == nil && len(refAgain.Chain) == 4 && refAgain.Terminal == ref.Terminal,
			"re-query after errors: err = %v", errAgain)

		// The returned reference owns its data: mutating the File afterwards
		// does not change it.
		f.COFFSymbols[6].Value = 0xdead
		f.COFFSymbols[0].Name[0] = 'X'
		f.Sections[0].Relocs[0].Type = 0xffff
		if len(ref.Chain) == 4 {
			c.Check(ref.Chain[3].Value == 1, "chain hop value changed to %d after mutation", ref.Chain[3].Value)
			c.Check(ref.Chain[0].Name == "weak_one", "chain hop name changed to %q after mutation", ref.Chain[0].Name)
		}
		c.Check(ref.Reloc.Type == 0x0004, "reloc type changed to 0x%x after mutation", ref.Reloc.Type)
	})
}

// TestTaskCompatibility checks that ordinary COFF objects and PE32/PE32+
// images keep working, that ordinary files are still written with 18-byte
// symbol records, and that BigObj files refuse writes.
func TestTaskCompatibility(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		// Ordinary COFF object from the fixture.
		raw := fixture.Basic(false)
		f, err := NewFile(bytes.NewReader(raw))
		c.Check(err == nil, "ordinary object: %v", err)
		if err == nil {
			c.Check(!f.IsBigObj(), "ordinary object must not be BigObj")
			c.Check(f.SectionCount() == 1 && f.FileHeader.NumberOfSections == 1,
				"section count = %d", f.SectionCount())
			c.Check(len(f.Symbols) == 1, "symbols = %d, want 1", len(f.Symbols))
			if len(f.Symbols) == 1 {
				c.Check(f.Symbols[0].Name == "anchor", "symbol = %q", f.Symbols[0].Name)
				c.Check(f.Symbols[0].SectionNumber == 1, "symbol section = %d", f.Symbols[0].SectionNumber)
			}
			c.Check(len(f.COFFSymbols) == 1, "raw slots = %d, want 1", len(f.COFFSymbols))
			if len(f.COFFSymbols) == 1 {
				c.Check(f.COFFSymbols[0].SectionNumber == 1, "raw slot section = %d", f.COFFSymbols[0].SectionNumber)
			}
			ref, rerr := f.COFFReference(1, 0)
			c.Check(rerr == nil, "ordinary COFFReference: %v", rerr)
			if rerr == nil {
				c.Check(len(ref.Chain) == 1 && ref.Chain[0].SlotIndex == 0,
					"chain = %+v", ref.Chain)
				c.Check(ref.Chain[0].Offset == f.FileHeader.PointerToSymbolTable,
					"slot offset = %d, want %d", ref.Chain[0].Offset, f.FileHeader.PointerToSymbolTable)
				c.Check(ref.Chain[0].Name == "anchor" && ref.Chain[0].StorageClass == 2,
					"hop = %q class %d", ref.Chain[0].Name, ref.Chain[0].StorageClass)
				c.Check(ref.Reloc.Type == 0x0004, "reloc type = 0x%x", ref.Reloc.Type)
				c.Check(ref.RelocationOffset == f.Sections[0].PointerToRelocations,
					"reloc offset = %d", ref.RelocationOffset)
				c.Check(binary.LittleEndian.Uint16(raw[ref.RelocationOffset+8:]) == 0x0004,
					"raw reloc type = 0x%x", binary.LittleEndian.Uint16(raw[ref.RelocationOffset+8:]))
				c.Check(ref.Terminal.Kind == COFFTerminalDefined && ref.Terminal.SectionIndex == 1,
					"terminal = %+v", ref.Terminal)
				c.Check(ref.Terminal.HasFileOffset && raw[ref.Terminal.FileOffset] == 0x90,
					"terminal file offset = %d has = %v", ref.Terminal.FileOffset, ref.Terminal.HasFileOffset)
			}
		}

		// Basic BigObj from the fixture.
		big, berr := NewFile(bytes.NewReader(fixture.Basic(true)))
		c.Check(berr == nil, "bigobj: %v", berr)
		if berr == nil {
			c.Check(big.IsBigObj(), "should be BigObj")
			c.Check(big.SectionCount() == 1, "bigobj section count = %d", big.SectionCount())
			c.Check(len(big.Symbols) == 1, "bigobj symbols = %d, want 1", len(big.Symbols))
			if len(big.Symbols) == 1 {
				c.Check(big.Symbols[0].Name == "long_anchor_name", "bigobj symbol = %q", big.Symbols[0].Name)
			}
			bref, brerr := big.COFFReference(1, 0)
			c.Check(brerr == nil, "bigobj reference: %v", brerr)
			if brerr == nil {
				c.Check(bref.Terminal.Kind == COFFTerminalDefined && bref.Terminal.SectionIndex == 1,
					"bigobj terminal = %+v", bref.Terminal)
			}
			// BigObj files are read-only for now.
			_, werr := big.Bytes()
			c.Check(werr != nil, "BigObj Bytes should fail")
			wferr := big.WriteFile(filepath.Join(t.TempDir(), "bigobj.bin"))
			c.Check(wferr != nil, "BigObj WriteFile should fail")
		}

		// A short import object shares the 00 00 ff ff signature but is not
		// a BigObj and must be rejected as such.
		imp := make([]byte, 64)
		binary.LittleEndian.PutUint16(imp[2:], 0xffff)
		binary.LittleEndian.PutUint16(imp[4:], 0) // import object version
		binary.LittleEndian.PutUint16(imp[6:], IMAGE_FILE_MACHINE_AMD64)
		_, ierr := NewFile(bytes.NewReader(imp))
		c.Check(ierr != nil && strings.Contains(ierr.Error(), "import"),
			"short import object error = %v", ierr)

		// A BigObj header for an unsupported machine is rejected as well.
		badMachine := buildTaskObject(true, nil, nil)
		binary.LittleEndian.PutUint16(badMachine[6:], IMAGE_FILE_MACHINE_I386)
		_, merr := NewFile(bytes.NewReader(badMachine))
		c.Check(merr != nil && strings.Contains(merr.Error(), "machine"),
			"non-AMD64 BigObj error = %v", merr)

		// Ordinary PE32 object still reads.
		obj, oerr := Open(path.Join("testdata", "gcc-386-mingw-obj"))
		c.Check(oerr == nil, "open PE32 object: %v", oerr)
		if oerr == nil {
			defer obj.Close()
			c.Check(!obj.IsBigObj() && obj.SectionCount() == 12,
				"PE32 object section count = %d", obj.SectionCount())
			c.Check(len(obj.Symbols) == 16, "PE32 object symbols = %d", len(obj.Symbols))
			if len(obj.Symbols) == 16 {
				c.Check(obj.Symbols[0].SectionNumber == -2, "first symbol section = %d", obj.Symbols[0].SectionNumber)
				c.Check(obj.Symbols[1].SectionNumber == 1, "second symbol section = %d", obj.Symbols[1].SectionNumber)
			}
		}

		// Ordinary PE32+ image still reads and writes with 18-byte symbol
		// records.
		exe, xerr := Open(path.Join("testdata", "gcc-amd64-mingw-exec"))
		c.Check(xerr == nil, "open PE32+ image: %v", xerr)
		if xerr == nil {
			defer exe.Close()
			c.Check(!exe.IsBigObj(), "PE32+ image must not be BigObj")
			c.Check(exe.SectionCount() == uint32(exe.FileHeader.NumberOfSections),
				"PE32+ section count = %d", exe.SectionCount())
			c.Check(len(exe.COFFSymbols) > 0, "PE32+ image should have COFF symbols")
			out, werr := exe.Bytes()
			c.Check(werr == nil, "PE32+ Bytes: %v", werr)
			if werr == nil {
				f2, rerr := NewFile(bytes.NewReader(out))
				c.Check(rerr == nil, "reopen written PE32+: %v", rerr)
				if rerr == nil {
					c.Check(len(f2.COFFSymbols) == len(exe.COFFSymbols),
						"written symbols = %d, want %d", len(f2.COFFSymbols), len(exe.COFFSymbols))
					c.Check(len(f2.Symbols) == len(exe.Symbols) && len(f2.Symbols) > 0,
						"written symbol count = %d, want %d", len(f2.Symbols), len(exe.Symbols))
					if len(f2.Symbols) == len(exe.Symbols) && len(f2.Symbols) > 0 {
						c.Check(f2.Symbols[0].Name == exe.Symbols[0].Name,
							"written first symbol = %q, want %q", f2.Symbols[0].Name, exe.Symbols[0].Name)
					}
					// The string table must start exactly 18 bytes per
					// symbol after PointerToSymbolTable.
					ptr := f2.FileHeader.PointerToSymbolTable
					n := f2.FileHeader.NumberOfSymbols
					strAt := ptr + COFFSymbolSize*n
					c.Check(int(strAt)+len(exe.StringTable) <= len(out),
						"string table offset %d beyond output %d", strAt, len(out))
					if int(strAt)+len(exe.StringTable) <= len(out) {
						c.Check(bytes.Equal(out[strAt:strAt+uint32(len(exe.StringTable))], exe.StringTable),
							"string table not found at 18-byte stride offset %d (18-byte records kept)", strAt)
					}
				}
			}
		}
	})
}
