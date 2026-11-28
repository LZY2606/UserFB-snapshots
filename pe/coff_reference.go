package pe

import (
	"encoding/binary"
	"fmt"
)

// IMAGE_SYM_CLASS_WEAK_EXTERNAL is the storage class of a canonical weak
// external symbol: an undefined symbol whose auxiliary record declares a
// fallback symbol to use when no other definition is linked.
const IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105

// Weak external auxiliary record search characteristics. Only these three
// declared search modes are recognized.
const (
	IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY = 1
	IMAGE_WEAK_EXTERN_SEARCH_LIBRARY   = 2
	IMAGE_WEAK_EXTERN_SEARCH_ALIAS     = 3
)

// coffRelocRecordSize is the on-disk size of one COFF relocation record.
const coffRelocRecordSize = 10

// COFFWeakAux describes the auxiliary record of a weak external symbol
// (storage class 105). TagIndex and Characteristics are kept exactly as
// declared; the record expresses a conditional fallback for the case that
// the symbol stays undefined, not an actual link result.
type COFFWeakAux struct {
	SlotIndex       uint32 // raw symbol table slot of the auxiliary record
	Offset          uint32 // file offset of the auxiliary record
	TagIndex        uint32 // raw slot index of the declared fallback symbol
	Characteristics uint32 // IMAGE_WEAK_EXTERN_SEARCH_* search mode
}

// COFFReferenceHop describes one raw symbol table slot visited while
// resolving a COFF relocation reference. SlotIndex addresses the original
// slot table including auxiliary records, so slot identity is preserved
// even for symbols that share a name.
type COFFReferenceHop struct {
	SlotIndex     uint32 // index into the raw COFF symbol table (auxiliary slots included)
	Offset        uint32 // file offset of the slot
	Name          string
	Value         uint32
	StorageClass  uint8
	SectionNumber int32        // full signed section number: 1-based, 0, -1 or -2
	Weak          *COFFWeakAux // non-nil when this hop is a weak external
}

// COFFTerminalKind classifies the final symbol of a reference chain.
type COFFTerminalKind int

const (
	COFFTerminalUndefined COFFTerminalKind = iota // section number 0, value 0
	COFFTerminalDefined                           // positive section number
	COFFTerminalCommon                            // section number 0, value > 0
	COFFTerminalAbsolute                          // section number -1
	COFFTerminalDebug                             // section number -2
)

// String returns a short description of the terminal kind.
func (k COFFTerminalKind) String() string {
	switch k {
	case COFFTerminalUndefined:
		return "undefined"
	case COFFTerminalDefined:
		return "defined"
	case COFFTerminalCommon:
		return "common"
	case COFFTerminalAbsolute:
		return "absolute"
	case COFFTerminalDebug:
		return "debug"
	}
	return "unknown"
}

// COFFReferenceTerminal describes the final, non-weak symbol of a
// reference chain. For COFFTerminalDefined it also gives the offset of the
// symbol value within its section and, when the value names an actual raw
// byte of that section, the corresponding file offset. No load address is
// computed.
type COFFReferenceTerminal struct {
	Kind          COFFTerminalKind
	SectionIndex  uint32 // 1-based section index, valid when Kind == COFFTerminalDefined
	SectionName   string // valid when Kind == COFFTerminalDefined
	SectionOffset uint32 // offset of the symbol value within the section
	FileOffset    uint32 // file offset of the referenced byte, valid when HasFileOffset
	HasFileOffset bool   // false for zero-fill or out-of-raw-range values
}

// COFFReference describes what a single COFF relocation refers to,
// resolved through the raw symbol table slots. The chain lists every
// visited slot in order; a weak external hop is followed by the slot its
// auxiliary record declares as fallback. The result is a static
// description of the declared fallback path, not a link result.
type COFFReference struct {
	SectionIndex     uint32 // 1-based index of the section holding the relocation
	SectionName      string
	RelocationIndex  uint32 // 0-based index into the section's Relocs
	RelocationOffset uint32 // file offset of the relocation record
	Reloc            Reloc  // the relocation record itself
	Chain            []COFFReferenceHop
	Terminal         COFFReferenceTerminal
}

// COFFReferenceError describes a failed COFFReference lookup. When the
// failure involves a raw symbol table slot, HasSlot is true and SlotIndex
// and SlotOffset identify that slot.
type COFFReferenceError struct {
	Reason     string
	HasSlot    bool
	SlotIndex  uint32 // raw slot index involved in the failure
	SlotOffset uint32 // file offset of the slot
}

func (e *COFFReferenceError) Error() string {
	if e.HasSlot {
		return fmt.Sprintf("pe: coff reference: %s (slot %d at file offset %d)", e.Reason, e.SlotIndex, e.SlotOffset)
	}
	return "pe: coff reference: " + e.Reason
}

// auxSlotSet returns the set of raw slot indices that hold auxiliary
// records rather than primary symbols.
func auxSlotSet(syms []COFFSymbol) map[uint32]bool {
	aux := make(map[uint32]bool)
	skip := 0
	for i := range syms {
		if skip > 0 {
			aux[uint32(i)] = true
			skip--
			continue
		}
		skip = int(syms[i].NumberOfAuxSymbols)
	}
	return aux
}

// COFFReference resolves the relocation relocationIndex (0-based) of
// section sectionIndex (1-based) through the raw COFF symbol table and
// reports what it refers to. SymbolTableIndex and weak-external TagIndex
// values address the original slot table including auxiliary records; the
// aux-stripped Symbols list is never indexed. Weak external hops follow
// the fallback declared by their auxiliary record (search characteristics
// 1, 2 and 3); the result describes that declared fallback path and does
// not select a symbol definition the way a linker would. The returned
// value owns its data and shares no mutable state with f.
func (f *File) COFFReference(sectionIndex, relocationIndex uint32) (*COFFReference, error) {
	if sectionIndex < 1 || sectionIndex > f.SectionCount() {
		return nil, &COFFReferenceError{Reason: fmt.Sprintf("section index %d out of range [1,%d]", sectionIndex, f.SectionCount())}
	}
	sec := f.Sections[sectionIndex-1]
	if relocationIndex >= uint32(len(sec.Relocs)) {
		return nil, &COFFReferenceError{Reason: fmt.Sprintf("relocation index %d out of range: section %d (%q) has %d relocations", relocationIndex, sectionIndex, sec.Name, len(sec.Relocs))}
	}
	rel := sec.Relocs[relocationIndex]
	ref := &COFFReference{
		SectionIndex:     sectionIndex,
		SectionName:      sec.Name,
		RelocationIndex:  relocationIndex,
		RelocationOffset: sec.PointerToRelocations + relocationIndex*coffRelocRecordSize,
		Reloc:            rel,
	}

	aux := auxSlotSet(f.COFFSymbols)
	stride := uint32(f.coffSymbolStride())
	slotOffset := func(idx uint32) uint32 {
		return f.FileHeader.PointerToSymbolTable + idx*stride
	}
	slotErr := func(idx uint32, format string, args ...interface{}) error {
		return &COFFReferenceError{
			Reason:     fmt.Sprintf(format, args...),
			HasSlot:    true,
			SlotIndex:  idx,
			SlotOffset: slotOffset(idx),
		}
	}

	visited := make(map[uint32]bool)
	idx := rel.SymbolTableIndex
	for {
		if idx >= uint32(len(f.COFFSymbols)) {
			return nil, slotErr(idx, "symbol table index %d out of range (%d slots)", idx, len(f.COFFSymbols))
		}
		if visited[idx] {
			return nil, slotErr(idx, "weak external fallback chain loops back to slot %d", idx)
		}
		visited[idx] = true
		if aux[idx] {
			return nil, slotErr(idx, "symbol table index %d points to an auxiliary slot", idx)
		}
		sym := f.COFFSymbols[idx]
		name, err := sym.FullName(f.StringTable)
		if err != nil {
			return nil, slotErr(idx, "cannot read symbol name: %v", err)
		}
		hop := COFFReferenceHop{
			SlotIndex:     idx,
			Offset:        slotOffset(idx),
			Name:          name,
			Value:         sym.Value,
			StorageClass:  sym.StorageClass,
			SectionNumber: sym.SectionNumber,
		}
		if sym.StorageClass == IMAGE_SYM_CLASS_WEAK_EXTERNAL {
			if sym.NumberOfAuxSymbols < 1 || idx+1 >= uint32(len(f.COFFSymbols)) {
				return nil, slotErr(idx, "weak external symbol has no auxiliary record")
			}
			auxSym := f.COFFSymbols[idx+1]
			weak := &COFFWeakAux{
				SlotIndex:       idx + 1,
				Offset:          slotOffset(idx + 1),
				TagIndex:        binary.LittleEndian.Uint32(auxSym.Name[0:4]),
				Characteristics: binary.LittleEndian.Uint32(auxSym.Name[4:8]),
			}
			switch weak.Characteristics {
			case IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY, IMAGE_WEAK_EXTERN_SEARCH_LIBRARY, IMAGE_WEAK_EXTERN_SEARCH_ALIAS:
			default:
				return nil, slotErr(idx, "weak external has unknown search characteristics %d", weak.Characteristics)
			}
			hop.Weak = weak
			ref.Chain = append(ref.Chain, hop)
			idx = weak.TagIndex
			continue
		}
		ref.Chain = append(ref.Chain, hop)
		term, err := f.classifyReferenceTerminal(&sym)
		if err != nil {
			return nil, slotErr(idx, "%v", err)
		}
		ref.Terminal = term
		return ref, nil
	}
}

// classifyReferenceTerminal classifies a non-weak terminal symbol by its
// section number.
func (f *File) classifyReferenceTerminal(sym *COFFSymbol) (COFFReferenceTerminal, error) {
	var term COFFReferenceTerminal
	switch {
	case sym.SectionNumber > 0:
		if uint32(sym.SectionNumber) > f.SectionCount() {
			return term, fmt.Errorf("symbol is defined in section %d but the file has %d sections", sym.SectionNumber, f.SectionCount())
		}
		sec := f.Sections[sym.SectionNumber-1]
		term.Kind = COFFTerminalDefined
		term.SectionIndex = uint32(sym.SectionNumber)
		term.SectionName = sec.Name
		term.SectionOffset = sym.Value
		// Report a file location only when the value names an actual raw
		// byte of the section; zero-fill and out-of-raw-range values stay
		// explicitly without one.
		if sec.Offset != 0 && sym.Value < sec.Size {
			term.HasFileOffset = true
			term.FileOffset = sec.Offset + sym.Value
		}
	case sym.SectionNumber == 0:
		if sym.Value > 0 {
			term.Kind = COFFTerminalCommon
		} else {
			term.Kind = COFFTerminalUndefined
		}
	case sym.SectionNumber == -1:
		term.Kind = COFFTerminalAbsolute
	case sym.SectionNumber == -2:
		term.Kind = COFFTerminalDebug
	default:
		return term, fmt.Errorf("symbol has invalid section number %d", sym.SectionNumber)
	}
	return term, nil
}
