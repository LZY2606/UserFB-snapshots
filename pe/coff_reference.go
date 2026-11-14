// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import (
	"encoding/binary"
	"fmt"
)

// COFF storage class for a canonical weak external symbol.
const IMAGE_SYM_CLASS_WEAK_EXTERNAL = 105

// relocSize is the on-disk size of one COFF relocation record:
// VirtualAddress (uint32), SymbolTableIndex (uint32), Type (uint16).
const relocSize = 10

// Weak external search characteristics, as stored in the weak-external
// auxiliary record. Only these three declared modes are understood.
const (
	IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY = 1
	IMAGE_WEAK_EXTERN_SEARCH_LIBRARY   = 2
	IMAGE_WEAK_EXTERN_SEARCH_ALIAS     = 3
)

// COFFTerminalKind classifies the tail of a COFF reference chain according
// to the terminal symbol's section number and value.
type COFFTerminalKind int

const (
	// COFFTerminalUndefined: section number 0 with value 0.
	COFFTerminalUndefined COFFTerminalKind = iota
	// COFFTerminalCommon: section number 0 with a non-zero value that
	// states the common allocation size.
	COFFTerminalCommon
	// COFFTerminalAbsolute: section number -1.
	COFFTerminalAbsolute
	// COFFTerminalDebug: section number -2.
	COFFTerminalDebug
	// COFFTerminalDefined: a positive section number plus an in-section
	// offset taken from the symbol value.
	COFFTerminalDefined
)

// COFFReferenceError reports a failed reference lookup. Slot-related
// failures carry the offending raw symbol-table index and its file
// position so that no half-built chain is returned without context.
type COFFReferenceError struct {
	Op         string
	SlotIndex  uint32 // raw, aux-inclusive slot index; zero when not slot-related
	SlotOffset uint32 // file position of that slot; zero when not slot-related
	Err        error
}

func (e *COFFReferenceError) Error() string {
	if e.SlotOffset != 0 || e.SlotIndex != 0 {
		return fmt.Sprintf("pe: coff reference %s: slot %d at file offset %#x: %v", e.Op, e.SlotIndex, e.SlotOffset, e.Err)
	}
	return fmt.Sprintf("pe: coff reference %s: %v", e.Op, e.Err)
}

func (e *COFFReferenceError) Unwrap() error { return e.Err }

// COFFWeakExternal describes the auxiliary record of a storage-class-105
// weak external symbol. The fields are the symbol's declared fallback
// relationship only; no library search or provider selection is performed.
type COFFWeakExternal struct {
	// AuxiliaryIndex is the raw slot index (aux-inclusive space) of the
	// auxiliary record, which immediately follows its owner.
	AuxiliaryIndex uint32
	// AuxiliaryOffset is the file position of that auxiliary slot.
	AuxiliaryOffset uint32
	// TagIndex is the raw slot index of the declared fallback symbol.
	TagIndex uint32
	// Characteristics is the original search characteristic (1, 2 or 3).
	Characteristics uint32
	// SearchName names the characteristic for readability.
	SearchName string
}

// COFFSymbolRef is one hop of a reference chain. It addresses a primary
// symbol through its raw, aux-inclusive slot identity, even when several
// slots share a name: equal names never merge distinct slots.
type COFFSymbolRef struct {
	SlotIndex          uint32
	Offset             uint32
	Name               string
	Value              uint32
	Type               uint16
	StorageClass       uint8
	SectionNumber      int32
	NumberOfAuxSymbols uint8
	// Weak is set only for storage class 105 weak external symbols.
	Weak *COFFWeakExternal
}

// COFFTerminal classifies the final slot of a reference chain. This is a
// static description of where the declared chain ends, not a link result.
type COFFTerminal struct {
	Kind COFFTerminalKind
	// SectionNumber is the terminal symbol's full signed section number.
	SectionNumber int32
	// SectionName names the defining section for COFFTerminalDefined.
	SectionName string
	// Value is the terminal symbol's raw value: the in-section offset for
	// a defined symbol or the declared size of a common symbol.
	Value uint32
	// DataOffset is the file position of the raw byte named by a defined
	// symbol's in-section offset. HasDataOffset is false when the value
	// falls outside the section's raw data (for example zero-fill .bss or
	// an offset beyond SizeOfRawData); no location is invented then.
	DataOffset    uint32
	HasDataOffset bool
}

// COFFReference explains which raw symbol slots one COFF relocation names.
// Every slice and string in the value is owned by the result: querying
// never borrows or mutates the File's symbol slices.
type COFFReference struct {
	// SectionIndex is the one-based section number supplied by the caller.
	SectionIndex uint32
	// RelocationIndex is the zero-based index within Section.Relocs.
	RelocationIndex uint32
	// RelocationOffset is the file position of the 10-byte relocation.
	RelocationOffset uint32
	// Relocation preserves the original VirtualAddress, SymbolTableIndex
	// and Type without applying any relocation arithmetic.
	Relocation Reloc
	// Chain lists every visited primary symbol slot, starting with the
	// slot named by the relocation and following weak-external TagIndex
	// values. Auxiliary slots are never chain entries, but their raw
	// positions are kept on the owning COFFSymbolRef.Weak.
	Chain []COFFSymbolRef
	// Terminal classifies the tail of the chain.
	Terminal COFFTerminal
}

func (f *File) slotOffset(index uint32) uint32 {
	return f.FileHeader.PointerToSymbolTable + index*f.symbolStride
}

func weakSearchName(characteristics uint32) string {
	switch characteristics {
	case IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY:
		return "SEARCH_NOLIBRARY"
	case IMAGE_WEAK_EXTERN_SEARCH_LIBRARY:
		return "SEARCH_LIBRARY"
	case IMAGE_WEAK_EXTERN_SEARCH_ALIAS:
		return "SEARCH_ALIAS"
	default:
		return ""
	}
}

// COFFReference traces the static symbol reference expressed by one COFF
// relocation. sectionIndex is a one-based section number and
// relocationIndex is the zero-based position within that section's Relocs
// slice. Both relocation SymbolTableIndex values and weak-external
// TagIndex values address the complete original slot table, auxiliary
// records included; the aux-free Symbols slice is never indexed here.
//
// The returned chain represents the fallback path the object declares for
// an absent symbol, not the outcome of linking. The input is never
// modified, relocations are never applied and object code is never run.
func (f *File) COFFReference(sectionIndex, relocationIndex uint32) (*COFFReference, error) {
	if sectionIndex == 0 || sectionIndex > f.SectionCount() {
		return nil, &COFFReferenceError{
			Op:  "locate section",
			Err: fmt.Errorf("section index %d is out of range (file has %d sections)", sectionIndex, f.SectionCount()),
		}
	}
	section := f.Sections[sectionIndex-1]
	if relocationIndex >= uint32(len(section.Relocs)) {
		return nil, &COFFReferenceError{
			Op:  "locate relocation",
			Err: fmt.Errorf("relocation index %d is out of range for section %d (it has %d relocations)", relocationIndex, sectionIndex, len(section.Relocs)),
		}
	}
	reloc := section.Relocs[relocationIndex]
	ref := &COFFReference{
		SectionIndex:     sectionIndex,
		RelocationIndex:  relocationIndex,
		RelocationOffset: section.PointerToRelocations + relocationIndex*uint32(relocSize),
		Relocation:       reloc,
		Chain:            make([]COFFSymbolRef, 0, 4),
	}

	visited := make(map[uint32]bool)
	next := reloc.SymbolTableIndex
	for {
		if next >= uint32(len(f.COFFSymbols)) {
			return nil, &COFFReferenceError{
				Op:         "follow symbol slot",
				SlotIndex:  next,
				SlotOffset: f.FileHeader.PointerToSymbolTable + next*f.symbolStride,
				Err:        fmt.Errorf("symbol slot index %d is beyond the table of %d slots", next, len(f.COFFSymbols)),
			}
		}
		if f.auxSlot[next] {
			return nil, &COFFReferenceError{
				Op:         "follow symbol slot",
				SlotIndex:  next,
				SlotOffset: f.slotOffset(next),
				Err:        fmt.Errorf("symbol slot index %d is an auxiliary record, not a primary symbol", next),
			}
		}
		if visited[next] {
			return nil, &COFFReferenceError{
				Op:         "follow weak fallback",
				SlotIndex:  next,
				SlotOffset: f.slotOffset(next),
				Err:        fmt.Errorf("weak external fallback chain loops back to slot %d", next),
			}
		}
		visited[next] = true

		sym := f.COFFSymbols[next]
		name, err := sym.FullName(f.StringTable)
		if err != nil {
			return nil, &COFFReferenceError{
				Op:         "resolve symbol name",
				SlotIndex:  next,
				SlotOffset: f.slotOffset(next),
				Err:        err,
			}
		}
		hop := COFFSymbolRef{
			SlotIndex:          next,
			Offset:             f.slotOffset(next),
			Name:               name,
			Value:              sym.Value,
			Type:               sym.Type,
			StorageClass:       sym.StorageClass,
			SectionNumber:      sym.SectionNumber,
			NumberOfAuxSymbols: sym.NumberOfAuxSymbols,
		}

		if sym.StorageClass != IMAGE_SYM_CLASS_WEAK_EXTERNAL {
			ref.Chain = append(ref.Chain, hop)
			terminal, err := f.classifyTerminal(&hop, &sym)
			if err != nil {
				return nil, err
			}
			ref.Terminal = *terminal
			return ref, nil
		}

		// Weak external: its single auxiliary record holds TagIndex and
		// the declared search characteristic.
		auxIndex := next + 1
		if sym.NumberOfAuxSymbols == 0 || auxIndex >= uint32(len(f.COFFSymbols)) || !f.auxSlot[auxIndex] {
			return nil, &COFFReferenceError{
				Op:         "read weak external auxiliary",
				SlotIndex:  next,
				SlotOffset: f.slotOffset(next),
				Err:        fmt.Errorf("weak external at slot %d is missing its auxiliary record", next),
			}
		}
		auxOffset := f.slotOffset(auxIndex)
		aux := make([]byte, f.symbolStride)
		if _, err := f.readerAt.ReadAt(aux, int64(auxOffset)); err != nil {
			return nil, &COFFReferenceError{
				Op:         "read weak external auxiliary",
				SlotIndex:  auxIndex,
				SlotOffset: auxOffset,
				Err:        err,
			}
		}
		tagIndex := binary.LittleEndian.Uint32(aux[0:4])
		characteristics := binary.LittleEndian.Uint32(aux[4:8])
		searchName := weakSearchName(characteristics)
		if searchName == "" {
			return nil, &COFFReferenceError{
				Op:         "read weak external auxiliary",
				SlotIndex:  auxIndex,
				SlotOffset: auxOffset,
				Err:        fmt.Errorf("unknown weak external search characteristic %d at slot %d", characteristics, auxIndex),
			}
		}
		hop.Weak = &COFFWeakExternal{
			AuxiliaryIndex:  auxIndex,
			AuxiliaryOffset: auxOffset,
			TagIndex:        tagIndex,
			Characteristics: characteristics,
			SearchName:      searchName,
		}
		ref.Chain = append(ref.Chain, hop)
		next = tagIndex
	}
}

// classifyTerminal turns the final primary slot into a static terminal
// classification, computing the raw-byte file position only when the
// symbol value names an actual byte of its section's raw data.
func (f *File) classifyTerminal(hop *COFFSymbolRef, sym *COFFSymbol) (*COFFTerminal, error) {
	t := &COFFTerminal{
		SectionNumber: sym.SectionNumber,
		Value:         sym.Value,
	}
	switch {
	case sym.SectionNumber > 0:
		if uint32(sym.SectionNumber) > f.SectionCount() {
			return nil, &COFFReferenceError{
				Op:         "classify defined symbol",
				SlotIndex:  hop.SlotIndex,
				SlotOffset: hop.Offset,
				Err:        fmt.Errorf("symbol defines section %d but the file only has %d sections", sym.SectionNumber, f.SectionCount()),
			}
		}
		section := f.Sections[sym.SectionNumber-1]
		t.Kind = COFFTerminalDefined
		t.SectionName = section.Name
		if section.Offset != 0 && sym.Value < section.Size {
			t.DataOffset = section.Offset + sym.Value
			t.HasDataOffset = true
		}
	case sym.SectionNumber == 0:
		if sym.Value > 0 {
			t.Kind = COFFTerminalCommon
		} else {
			t.Kind = COFFTerminalUndefined
		}
	case sym.SectionNumber == -1:
		t.Kind = COFFTerminalAbsolute
	case sym.SectionNumber == -2:
		t.Kind = COFFTerminalDebug
	default:
		return nil, &COFFReferenceError{
			Op:         "classify terminal symbol",
			SlotIndex:  hop.SlotIndex,
			SlotOffset: hop.Offset,
			Err:        fmt.Errorf("unrecognised special section number %d", sym.SectionNumber),
		}
	}
	return t, nil
}
