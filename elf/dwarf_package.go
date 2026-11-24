// Copyright 2025 The Binject Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// DWARF v5 split-DWARF package (.dwp) support.
//
// This file implements a strictly read-only parser for the version 5
// .debug_cu_index table carried by a DWARF package. It only interprets the
// 64-bit CU index layout (DWARF32 packages, little-endian ELF64 inputs).
// TU indexes, GNU version 2 indexes and relocations are rejected explicitly.
package elf

import (
	"encoding/binary"
	"fmt"
)

// DWP section identifier values used by the version 5 column table, per the
// DWARF 5 specification (section 7.3.5.2).
const (
	dwpSecInfo       uint32 = 1  // .debug_info.dwo
	dwpSecTypes      uint32 = 2  // .debug_types.dwo
	dwpSecAbbrev     uint32 = 3  // .debug_abbrev.dwo
	dwpSecLine       uint32 = 4  // .debug_line.dwo
	dwpSecLoclists   uint32 = 5  // .debug_loclists.dwo
	dwpSecStrOffsets uint32 = 6  // .debug_str_offsets.dwo
	dwpSecStr        uint32 = 7  // .debug_str.dwo, shared package wide
	dwpSecRnglists   uint32 = 8  // .debug_rnglists.dwo
	dwpSecMacinfo    uint32 = 9  // .debug_macinfo.dwo
	dwpSecMacro      uint32 = 10 // .debug_macro.dwo
)

// dwpSectionName maps a version 5 column identifier to its ELF section name.
var dwpSectionName = map[uint32]string{
	dwpSecInfo:       ".debug_info.dwo",
	dwpSecTypes:      ".debug_types.dwo",
	dwpSecAbbrev:     ".debug_abbrev.dwo",
	dwpSecLine:       ".debug_line.dwo",
	dwpSecLoclists:   ".debug_loclists.dwo",
	dwpSecStrOffsets: ".debug_str_offsets.dwo",
	dwpSecStr:        ".debug_str.dwo",
	dwpSecRnglists:   ".debug_rnglists.dwo",
	dwpSecMacinfo:    ".debug_macinfo.dwo",
	dwpSecMacro:      ".debug_macro.dwo",
}

// DWARF 32-bit unit_length values must be below this bound; values at or above
// it signal the 64-bit DWARF format, which this parser does not support.
const dwarf32MaxLength uint32 = 0xfffffff0

// DWPContribution identifies one unit's contribution to a single package
// section. All offsets are byte offsets.
type DWPContribution struct {
	// Section is the version 5 column identifier (see the dwpSec* constants).
	Section uint32 `json:"section"`
	// SectionName is the ELF section holding the contribution.
	SectionName string `json:"section_name"`
	// Offset is the contribution start relative to the start of SectionName.
	Offset uint64 `json:"offset"`
	// Size is the contribution length in bytes.
	Size uint64 `json:"size"`
	// FileOffset is the contribution start as an absolute offset in the
	// package file, i.e. the ELF section header offset plus Offset.
	FileOffset uint64 `json:"file_offset"`
}

// DWPUnknownColumn preserves a column in the CU index whose section identifier
// this implementation does not know how to interpret. The fields mirror the
// raw offset/size pair of the column for the owning row.
type DWPUnknownColumn struct {
	ID         uint32 `json:"id"`
	Offset     uint64 `json:"offset"`
	Size       uint64 `json:"size"`
	FileOffset uint64 `json:"file_offset"`
}

// DWPUnit is one indexed split compile unit inside a DWARF package.
type DWPUnit struct {
	// Signature is the 64-bit DWO identifier (DW_AT_dwo_id) of the unit.
	Signature uint64
	// IndexRow is the 1-based row number in the .debug_cu_index tables.
	// It is never zero for a unit returned by a successful lookup.
	IndexRow uint32
	// Contributions maps each known version 5 section identifier to the
	// unit's contribution. A zero-size rnglists contribution means the unit
	// does not contribute to .debug_rnglists.dwo.
	Contributions map[uint32]*DWPContribution
	// UnknownColumns retains columns whose identifiers are not part of the
	// version 5 identifier set understood by this parser.
	UnknownColumns []DWPUnknownColumn
}

// DWPIndexMissReason explains why a Lookup did not resolve an identifier.
type DWPIndexMissReason int

const (
	// DWPMissEmptySlot means the probe sequence reached an unused hash
	// bucket, which proves the identifier is not indexed.
	DWPMissEmptySlot DWPIndexMissReason = iota + 1
	// DWPMissTableFull means every bucket in the table was probed without
	// finding the identifier or an empty slot, so the identifier cannot be
	// present in a well-formed table.
	DWPMissTableFull
)

// DWPIndexMiss is the distinguishable negative result of DWARFPackage.Lookup.
type DWPIndexMiss struct {
	// ID is the DWO identifier that was searched for.
	ID uint64
	// Reason classifies how the probe sequence terminated.
	Reason DWPIndexMissReason
	// TerminatingBucket is the hash bucket at which probing stopped.
	TerminatingBucket uint32
	// Probes is the number of hash buckets examined before termination.
	Probes int
}

// DWPIndexResult is the result of DWARFPackage.Lookup. Call Found to tell a
// hit from a miss apart; a miss never has a non-nil Unit.
type DWPIndexResult struct {
	// Unit is the indexed unit on a hit, and nil on a miss.
	Unit *DWPUnit
	// Miss is nil on a hit and describes the terminated probe sequence on a
	// miss.
	Miss *DWPIndexMiss
}

// Found reports whether the lookup resolved an indexed unit.
func (r DWPIndexResult) Found() bool { return r.Unit != nil }

// DWARFPackage is a parsed, read-only view of a DWARF version 5 package.
//
// The package owns copies of every section byte it needs, so closing the
// underlying ELF file or overwriting the buffers the file was built from does
// not affect later lookups. A single DWARFPackage is safe to reuse across
// multiple images.
type DWARFPackage struct {
	byteOrder binary.ByteOrder

	// Raw index geometry, kept for diagnostics and tests.
	bucketCount uint32
	bucketSig   []uint64
	bucketRow   []uint32

	units       []*DWPUnit
	rowBySig    map[uint64]*DWPUnit
	sections    map[string]*Section
	sectionData map[string][]byte // owned copies, keyed by section name
}

// DWARFPackage parses the .debug_cu_index table of f and returns a reusable
// package view. f must be an uncompressed, little-endian ELF64 DWARF package
// using the version 5 CU index layout.
//
// Repeated calls on the same File return the same *DWARFPackage.
func (f *File) DWARFPackage() (*DWARFPackage, error) {
	if f.dwpCache != nil {
		return f.dwpCache, f.dwpCacheErr
	}
	p, err := buildDWARFPackage(f)
	f.dwpCache = p
	f.dwpCacheErr = err
	return p, err
}

func buildDWARFPackage(f *File) (pkg *DWARFPackage, err error) {
	if f.Class != ELFCLASS64 {
		return nil, &SplitError{Object: "dwp", Op: "open", Err: fmt.Errorf("only ELF64 packages are supported, got class %v", f.Class)}
	}
	if f.Data != ELFDATA2LSB {
		return nil, &SplitError{Object: "dwp", Op: "open", Err: fmt.Errorf("only little-endian packages are supported, got %v", f.Data)}
	}
	if f.Section(".debug_tu_index") != nil {
		return nil, &SplitError{Object: "dwp", Section: ".debug_tu_index", Op: "open",
			Err: errUnsupported("TU indexes are not supported")}
	}
	for _, s := range f.Sections {
		if s.Type == SHT_REL || s.Type == SHT_RELA {
			return nil, &SplitError{Object: "dwp", Section: s.Name, Offset: int64(s.Offset), Op: "open",
				Err: errUnsupported("relocated debug sections are not supported")}
		}
		if s.Flags&SHF_COMPRESSED != 0 {
			return nil, &SplitError{Object: "dwp", Section: s.Name, Offset: int64(s.Offset), Op: "open",
				Err: errUnsupported("compressed debug sections are not supported")}
		}
	}

	indexSection := f.Section(".debug_cu_index")
	if indexSection == nil {
		return nil, &SplitError{Object: "dwp", Op: "open", Err: fmt.Errorf("missing .debug_cu_index section")}
	}
	indexData, err := cloneSection(indexSection)
	if err != nil {
		return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index", Offset: int64(indexSection.Offset), Op: "read", Err: err}
	}
	if len(indexData) < 16 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index", Offset: int64(indexSection.Offset), Op: "parse header",
			Err: fmt.Errorf("truncated CU index: %d bytes", len(indexData))}
	}
	bo := f.ByteOrder
	version := bo.Uint32(indexData[0:])
	if version != 5 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index", Offset: int64(indexSection.Offset), Op: "parse header",
			Err: errUnsupported("only CU index version 5 is supported, got version %d", version)}
	}
	numColumns := bo.Uint32(indexData[4:])
	numUnits := bo.Uint32(indexData[8:])
	bucketCount := bo.Uint32(indexData[12:])
	if numColumns == 0 || numUnits == 0 || bucketCount == 0 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index", Offset: int64(indexSection.Offset), Op: "parse header",
			Err: fmt.Errorf("invalid CU index dimensions columns=%d units=%d buckets=%d", numColumns, numUnits, bucketCount)}
	}
	if bucketCount&(bucketCount-1) != 0 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index", Offset: int64(indexSection.Offset), Op: "parse header",
			Err: fmt.Errorf("CU index bucket count %d is not a power of two", bucketCount)}
	}
	need := uint64(16) + uint64(bucketCount)*12 + uint64(numColumns)*4 + uint64(numColumns)*uint64(numUnits)*8
	if uint64(len(indexData)) < need {
		return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index", Offset: int64(indexSection.Offset), Op: "parse header",
			Err: fmt.Errorf("CU index needs %d bytes but section has %d", need, len(indexData))}
	}

	p := &DWARFPackage{
		byteOrder:   bo,
		bucketCount: bucketCount,
		bucketSig:   make([]uint64, bucketCount),
		bucketRow:   make([]uint32, bucketCount),
		rowBySig:    map[uint64]*DWPUnit{},
		sections:    map[string]*Section{},
		sectionData: map[string][]byte{},
	}

	pos := 16
	for b := uint32(0); b < bucketCount; b++ {
		p.bucketSig[b] = bo.Uint64(indexData[pos:])
		pos += 8
	}
	for b := uint32(0); b < bucketCount; b++ {
		p.bucketRow[b] = bo.Uint32(indexData[pos:])
		pos += 4
	}
	columnIDs := make([]uint32, numColumns)
	for c := range columnIDs {
		columnIDs[c] = bo.Uint32(indexData[pos:])
		pos += 4
	}
	tableCells := int(numColumns) * int(numUnits)
	offsetTable := indexData[pos : pos+4*tableCells]
	pos += 4 * tableCells
	sizeTable := indexData[pos : pos+4*tableCells]

	// The package-wide string section is shared by every unit and is not
	// itself a CU index column, so load it unconditionally.
	for _, name := range []string{".debug_str.dwo"} {
		s := f.Section(name)
		if s == nil {
			return nil, &SplitError{Object: "dwp", Section: name, Op: "open",
				Err: fmt.Errorf("package-wide %s section is absent", name)}
		}
		data, err := cloneSection(s)
		if err != nil {
			return nil, &SplitError{Object: "dwp", Section: name, Offset: int64(s.Offset), Op: "read", Err: err}
		}
		p.sections[name] = s
		p.sectionData[name] = data
	}
	// Resolve and own the bytes of every section referenced by a column.
	needed := map[uint32]bool{}
	for _, id := range columnIDs {
		needed[id] = true
	}
	for id := range needed {
		name, known := dwpSectionName[id]
		if !known || name == ".debug_str.dwo" {
			continue // retained as an unknown column per row, or loaded above
		}
		s := f.Section(name)
		if s == nil {
			return nil, &SplitError{Object: "dwp", Section: name, Op: "open",
				Err: fmt.Errorf("CU index references section %q which is absent", name)}
		}
		data, err := cloneSection(s)
		if err != nil {
			return nil, &SplitError{Object: "dwp", Section: name, Offset: int64(s.Offset), Op: "read", Err: err}
		}
		p.sections[name] = s
		p.sectionData[name] = data
	}
	if data := p.sectionData[".debug_str.dwo"]; len(data) == 0 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_str.dwo", Op: "open",
			Err: fmt.Errorf("package-wide .debug_str.dwo is empty")}
	}

	for r := uint32(0); r < numUnits; r++ {
		unit := &DWPUnit{
			IndexRow:      r + 1, // rows are 1-based in the index
			Contributions: map[uint32]*DWPContribution{},
		}
		for c, id := range columnIDs {
			cell := int(r)*int(numColumns) + c
			off := uint64(bo.Uint32(offsetTable[cell*4:]))
			size := uint64(bo.Uint32(sizeTable[cell*4:]))
			name, known := dwpSectionName[id]
			if !known {
				unit.UnknownColumns = append(unit.UnknownColumns, DWPUnknownColumn{
					ID: id, Offset: off, Size: size,
				})
				continue
			}
			section := p.sections[name]
			if off+size > uint64(len(p.sectionData[name])) {
				return nil, &SplitError{Object: "dwp", Section: name,
					Offset: int64(section.Offset + off), Op: "bounds check",
					Err: fmt.Errorf("row %d contribution [off=%#x size=%#x] exceeds section size %#x", r+1, off, size, len(p.sectionData[name]))}
			}
			if size == 0 {
				// A zero contribution (e.g. a unit without rnglists) is legal;
				// keep it recorded so callers can tell absence apart from data.
				unit.Contributions[id] = &DWPContribution{
					Section: id, SectionName: name, Offset: off, Size: 0,
					FileOffset: section.Offset + off,
				}
				continue
			}
			unit.Contributions[id] = &DWPContribution{
				Section:     id,
				SectionName: name,
				Offset:      off,
				Size:        size,
				FileOffset:  section.Offset + off,
			}
		}
		// Fill file offsets for unknown columns when the section happens to
		// exist, while keeping the identifier opaque.
		for i := range unit.UnknownColumns {
			uc := &unit.UnknownColumns[i]
			if name, ok := dwpSectionName[uc.ID]; ok {
				if s := f.Section(name); s != nil {
					uc.FileOffset = s.Offset + uc.Offset
				}
			}
		}
		p.units = append(p.units, unit)
	}

	// Cross-check the open-addressing hash table: every occupied bucket must
	// name a valid row, its signature must be reachable through the documented
	// probe sequence, and no signature or row may appear twice.
	seenSigs := map[uint64]bool{}
	seenRows := map[uint32]bool{}
	for b := uint32(0); b < bucketCount; b++ {
		sig := p.bucketSig[b]
		row := p.bucketRow[b]
		if sig == 0 {
			if row != 0 {
				return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index",
					Offset: int64(indexSection.Offset) + int64(16+8*int(bucketCount)+4*int(b)),
					Op:     "parse hash table", Err: fmt.Errorf("empty bucket %d carries row index %d", b, row)}
			}
			continue
		}
		if row == 0 || row > numUnits {
			return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index",
				Offset: int64(indexSection.Offset) + int64(16+8*int(bucketCount)+4*int(b)),
				Op:     "parse hash table", Err: fmt.Errorf("bucket %d has invalid row %d (units=%d)", b, row, numUnits)}
		}
		if seenSigs[sig] {
			return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index",
				Offset: int64(indexSection.Offset), Op: "parse hash table",
				Err: fmt.Errorf("duplicate DWO signature %#016x in hash table", sig)}
		}
		if seenRows[row] {
			return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index",
				Offset: int64(indexSection.Offset), Op: "parse hash table",
				Err: fmt.Errorf("CU index row %d is referenced by more than one bucket", row)}
		}
		seenSigs[sig] = true
		seenRows[row] = true
		res := p.probe(sig)
		if !res.Found() || res.Unit.IndexRow != row {
			return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index",
				Offset: int64(indexSection.Offset), Op: "parse hash table",
				Err: fmt.Errorf("signature %#016x is not reachable from its hash probe sequence", sig)}
		}
	}
	// Attach signatures to the row-oriented units; every row must be indexed.
	for r, unit := range p.units {
		unit.Signature = 0
		for b := uint32(0); b < bucketCount; b++ {
			if p.bucketRow[b] == uint32(r)+1 {
				unit.Signature = p.bucketSig[b]
				p.rowBySig[unit.Signature] = unit
				break
			}
		}
		if unit.Signature == 0 {
			return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index",
				Offset: int64(indexSection.Offset), Op: "parse hash table",
				Err: fmt.Errorf("row %d is not referenced by any hash bucket", unit.IndexRow)}
		}
	}
	return p, nil
}

// probe runs the version 5 double-hashing open-addressing lookup. A DWO
// identifier is itself a 64-bit hash, so the two functions use its halves:
// the home bucket is the full signature modulo the bucket count, while the
// probe stride is derived from the upper 32 bits and forced odd so it is
// always coprime with the power-of-two bucket count. An empty bucket or a
// fully probed table terminates the search.
func (p *DWARFPackage) probe(id uint64) DWPIndexResult {
	mask := p.bucketCount - 1
	home := uint32(id & uint64(mask))
	var stride uint32 = 1
	if p.bucketCount > 1 {
		stride = uint32((id>>32)%(uint64(p.bucketCount)-1)) | 1
	}
	cur := home
	for probes := 1; probes <= int(p.bucketCount); probes++ {
		sig := p.bucketSig[cur]
		if sig == 0 {
			return DWPIndexResult{Miss: &DWPIndexMiss{
				ID: id, Reason: DWPMissEmptySlot,
				TerminatingBucket: cur, Probes: probes,
			}}
		}
		if sig == id {
			return DWPIndexResult{Unit: p.units[p.bucketRow[cur]-1]}
		}
		cur = (cur + stride) & mask
	}
	return DWPIndexResult{Miss: &DWPIndexMiss{
		ID: id, Reason: DWPMissTableFull,
		TerminatingBucket: cur, Probes: int(p.bucketCount),
	}}
}

// Lookup returns the indexed unit carrying id, or a distinguishable miss if
// the package index does not contain it. The result never has both a unit and
// a miss set.
func (p *DWARFPackage) Lookup(id uint64) DWPIndexResult {
	if unit, ok := p.rowBySig[id]; ok {
		return DWPIndexResult{Unit: unit}
	}
	// Keep the explicit probe walk even on the miss path so callers get a
	// real, terminating probe record rather than a map failure.
	return p.probe(id)
}

// sectionBytes returns the package-owned bytes of section name. The returned
// slice must not be modified by callers.
func (p *DWARFPackage) sectionBytes(name string) ([]byte, bool) {
	b, ok := p.sectionData[name]
	return b, ok
}

// cloneSection reads a section into a freshly allocated buffer so that later
// mutation of any caller-owned input bytes cannot change analysis results.
func cloneSection(s *Section) ([]byte, error) {
	b, err := s.Data()
	if err != nil {
		return nil, err
	}
	out := make([]byte, len(b))
	copy(out, b)
	return out, nil
}

// errUnsupported marks format variants this read-only chain deliberately
// refuses to interpret instead of producing a plausible but wrong answer.
func errUnsupported(format string, args ...interface{}) error {
	return fmt.Errorf("unsupported split DWARF format: "+format, args...)
}
