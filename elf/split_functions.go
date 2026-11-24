// Copyright 2025 The Binject Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

// Read-only recovery of split-DWARF function attribution.
//
// SplitFunctions pairs the DW_UT_skeleton units retained in a linked ELF
// image with the DW_UT_split_compile units carried by a DWARF package, using
// the 64-bit DWO identifier as the only pairing key. It supports uncompressed,
// linked, little-endian ELF64 images using DWARF32 version 5, with function
// ranges expressed either as a DW_FORM_addrx* DW_AT_low_pc plus a constant
// DW_AT_high_pc, or as DW_FORM_rnglistx DW_AT_ranges referencing
// DW_RLE_offset_pair / DW_RLE_end_of_list range lists. Everything else that
// would require guessing an address is rejected with a contextual SplitError
// instead of being approximated.
package elf

import (
	"debug/dwarf"
	"encoding/binary"
	"fmt"
)

// DWARF unit types and forms used by the recovery chain.
const (
	utSkeleton     byte = 0x04
	utSplitCompile byte = 0x05

	formAddr          uint32 = 0x01
	formData2         uint32 = 0x05
	formData4         uint32 = 0x06
	formData8         uint32 = 0x07
	formData1         uint32 = 0x0b
	formSData         uint32 = 0x0d
	formUData         uint32 = 0x0f
	formSecOffset     uint32 = 0x17
	formExprLoc       uint32 = 0x18
	formFlagPresent   uint32 = 0x19
	formStrx          uint32 = 0x1a
	formAddrx         uint32 = 0x1b
	formRef4          uint32 = 0x13
	formRnglistx      uint32 = 0x23
	formStrx1         uint32 = 0x25
	formStrx2         uint32 = 0x26
	formStrx3         uint32 = 0x27
	formStrx4         uint32 = 0x28
	formAddrx1        uint32 = 0x29
	formAddrx2        uint32 = 0x2a
	formAddrx3        uint32 = 0x2b
	formAddrx4        uint32 = 0x2c
	formImplicitConst uint32 = 0x21

	rleEndOfList  byte = 0x00
	rleOffsetPair byte = 0x04
)

// SplitError is the only error type returned by the split-DWARF recovery
// chain. It records the object (image or package), section and byte offset at
// which a problem was found, so a failure can never be mistaken for a partial
// but plausible result.
type SplitError struct {
	// Object is "image" for the linked ELF or "dwp" for the package.
	Object string
	// Section is the ELF section involved, or "" when the error is not
	// section specific.
	Section string
	// Offset is a section-relative byte offset when known.
	Offset int64
	// FileOffset is an absolute file offset when known.
	FileOffset int64
	// Op names the failing operation, e.g. "parse header".
	Op string
	// DWOID is the DWO identifier being processed, when applicable.
	DWOID uint64
	// Err is the underlying error.
	Err error
}

func (e *SplitError) Error() string {
	s := "split DWARF " + e.Op + " failed"
	if e.Object != "" {
		s += " in " + e.Object
	}
	if e.Section != "" {
		s += " section " + e.Section
	}
	if e.Offset != 0 {
		s += fmt.Sprintf(" at section offset %#x", e.Offset)
	}
	if e.FileOffset != 0 {
		s += fmt.Sprintf(" (file offset %#x)", e.FileOffset)
	}
	if e.DWOID != 0 {
		s += fmt.Sprintf(" for DWO %#016x", e.DWOID)
	}
	if e.Err != nil {
		s += ": " + e.Err.Error()
	}
	return s
}

// Unwrap exposes the underlying error for errors.Is/errors.As.
func (e *SplitError) Unwrap() error { return e.Err }

// SplitPosition is a byte position stated simultaneously relative to its
// section and relative to the whole file. Object says which input the bytes
// live in, so a position from the image can never be confused with one from
// the package.
type SplitPosition struct {
	Object        string `json:"object"`
	Section       string `json:"section"`
	SectionOffset uint64 `json:"section_offset"`
	FileOffset    uint64 `json:"file_offset"`
}

// SplitRange is one half-open [Low, High) link-time PC interval.
type SplitRange struct {
	Low  uint64 `json:"low"`
	High uint64 `json:"high"`
}

// SplitAddrEvidence records the concrete .debug_addr entry that resolved an
// addrx form. The entry position always belongs to the image; Base is the
// skeleton's own DW_AT_addr_base.
type SplitAddrEvidence struct {
	// Index is the raw addrx index decoded from the DIE.
	Index uint64 `json:"index"`
	// Base is the skeleton DW_AT_addr_base applied to the index.
	Base uint64 `json:"addr_base"`
	// TableEntry is the position of the address entry that was read.
	TableEntry SplitPosition `json:"table_entry"`
	// Value is the address stored in that entry.
	Value uint64 `json:"value"`
}

// SplitRnglistEvidence records the rnglistx index, its offset-table entry and
// the first byte of the referenced range list. All three positions belong to
// the package.
type SplitRnglistEvidence struct {
	// Index is the raw rnglistx index decoded from the DIE.
	Index uint64 `json:"index"`
	// OffsetTableEntry is the position of the rnglist offset table entry.
	OffsetTableEntry SplitPosition `json:"offset_table_entry"`
	// List is the position of the first opcode byte of the referenced list.
	List SplitPosition `json:"list"`
	// BasePC is the paired skeleton DW_AT_low_pc used to resolve
	// DW_RLE_offset_pair operands.
	BasePC uint64 `json:"base_pc"`
}

// SplitFunction is one subprogram with at least one resolvable code range.
// Function identity is the DWO identifier plus the function DIE's original
// position; same-named local functions in different units stay separate.
type SplitFunction struct {
	// Name is the subprogram's DW_AT_name.
	Name string `json:"name"`
	// CompileUnitName is the split compile unit root's DW_AT_name.
	CompileUnitName string `json:"compile_unit_name"`
	// SkeletonCompDir is the skeleton's DW_AT_comp_dir.
	SkeletonCompDir string `json:"skeleton_comp_dir"`
	// DWOID is the 64-bit identifier shared by the skeleton and the split
	// unit.
	DWOID uint64 `json:"dwo_id"`
	// IndexRow is the 1-based row of the unit in the package CU index.
	IndexRow uint32 `json:"index_row"`
	// Ranges are the fully resolved, half-open link-time PC intervals of the
	// function. A function described by several ranges keeps one record with
	// several entries.
	Ranges []SplitRange `json:"ranges"`

	// SkeletonHeader is the first byte of the skeleton unit header in the
	// image's .debug_info.
	SkeletonHeader SplitPosition `json:"skeleton_header"`
	// SkeletonRootDIE is the skeleton root DIE position in the image.
	SkeletonRootDIE SplitPosition `json:"skeleton_root_die"`

	// FunctionDIELocal is the function DIE position relative to its
	// .debug_info.dwo contribution.
	FunctionDIELocal SplitPosition `json:"function_die_local"`
	// FunctionDIESection is the same position relative to the whole
	// .debug_info.dwo section.
	FunctionDIESection SplitPosition `json:"function_die_section"`
	// FunctionDIEFile restates the absolute package file offset.
	FunctionDIEFile SplitPosition `json:"function_die_file"`

	// Contributions records the package section contributions that
	// participated in recovery: INFO, ABBREV, STR_OFFSETS and RNGLISTS (the
	// latter has Size 0 when the unit has no rnglists contribution).
	Contributions map[uint32]DWPContribution `json:"contributions"`

	// Addr is present when the function used an addrx form.
	Addr *SplitAddrEvidence `json:"addr_evidence,omitempty"`
	// Rnglist is present when the function used an rnglistx form.
	Rnglist *SplitRnglistEvidence `json:"rnglist_evidence,omitempty"`
}

// addrBlock describes one per-skeleton .debug_addr block in the image.
type addrBlock struct {
	base      uint64 // section offset of the first address entry
	end       uint64 // section offset one past the block
	fileStart uint64 // file offset of the block header
}

// skeletonUnit is the manually parsed skeleton header plus the attributes
// decoded through debug/dwarf.
type skeletonUnit struct {
	dwoID      uint64
	headerOff  uint64
	length     uint64
	rootDIEOff uint64
	abbrevOff  uint64
	lowPC      uint64
	highPC     int64
	addrBase   uint64
	compDir    string
	dwoName    string
	addr       addrBlock
}

// SplitFunctions recovers every subprogram that owns code ranges in the
// linked image f, using the split compile units indexed by pkg.
//
// Pairing is performed solely by DWO identifier: each skeleton header's
// 64-bit DW_AT_dwo_id is matched against the package CU index and then
// checked against the split unit header. CU order, dwo_name, function names
// and source file names never influence pairing. On any structural problem
// the returned slice is nil and the error is a *SplitError naming the object,
// section and offset involved.
func (f *File) SplitFunctions(pkg *DWARFPackage) ([]SplitFunction, error) {
	if pkg == nil {
		return nil, &SplitError{Object: "image", Op: "open", Err: fmt.Errorf("DWARF package is required")}
	}
	if err := f.checkSplitInput(); err != nil {
		return nil, err
	}

	imgInfo, err := readOwnedSection(f, ".debug_info")
	if err != nil {
		return nil, err
	}
	imgAbbrev, err := readOwnedSection(f, ".debug_abbrev")
	if err != nil {
		return nil, err
	}
	imgStr, err := readOwnedSection(f, ".debug_str")
	if err != nil {
		return nil, err
	}
	imgStrOff, err := readOwnedSection(f, ".debug_str_offsets")
	if err != nil {
		return nil, err
	}
	imgAddr, err := readOwnedSection(f, ".debug_addr")
	if err != nil {
		return nil, err
	}
	infoSec := f.Section(".debug_info")
	addrSec := f.Section(".debug_addr")

	skels, err := parseSkeletons(f.ByteOrder, imgInfo, imgAbbrev, infoSec)
	if err != nil {
		return nil, err
	}
	if err := checkAddrBlocks(imgAddr, addrSec); err != nil {
		return nil, err
	}

	// Skeleton attributes through the standard library reader. The image
	// sections carry explicit str_offsets_base/addr_base values, so the full
	// sections can be handed over unchanged.
	skelDW, err := dwarf.New(imgAbbrev, nil, nil, imgInfo, nil, nil, nil, imgStr)
	if err != nil {
		return nil, &SplitError{Object: "image", Section: ".debug_info", Op: "parse skeleton DIEs", Err: err}
	}
	if err := skelDW.AddSection(".debug_str_offsets", imgStrOff); err != nil {
		return nil, &SplitError{Object: "image", Section: ".debug_str_offsets", Op: "parse skeleton DIEs", Err: err}
	}
	if err := skelDW.AddSection(".debug_addr", imgAddr); err != nil {
		return nil, &SplitError{Object: "image", Section: ".debug_addr", Op: "parse skeleton DIEs", Err: err}
	}
	skelEntries := map[dwarf.Offset]*dwarf.Entry{}
	sr := skelDW.Reader()
	for {
		e, err := sr.Next()
		if err != nil {
			return nil, &SplitError{Object: "image", Section: ".debug_info", Op: "parse skeleton DIEs", Err: err}
		}
		if e == nil {
			break
		}
		skelEntries[e.Offset] = e
	}
	for i := range skels {
		s := &skels[i]
		e := skelEntries[dwarf.Offset(s.rootDIEOff)]
		if e == nil || e.Tag != dwarf.TagSkeletonUnit {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(s.rootDIEOff), FileOffset: int64(infoSec.Offset + s.rootDIEOff),
				Op: "parse skeleton DIEs", DWOID: s.dwoID,
				Err: fmt.Errorf("root DIE is not a DW_UT_skeleton unit")}
		}
		lowPC, ok := e.Val(dwarf.AttrLowpc).(uint64)
		if !ok {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(s.rootDIEOff), Op: "parse skeleton DIEs", DWOID: s.dwoID,
				Err: fmt.Errorf("skeleton lacks a DW_AT_low_pc address")}
		}
		s.lowPC = lowPC
		switch v := e.Val(dwarf.AttrHighpc).(type) {
		case int64:
			s.highPC = v
		case uint64:
			s.highPC = int64(v)
		default:
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(s.rootDIEOff), Op: "parse skeleton DIEs", DWOID: s.dwoID,
				Err: fmt.Errorf("skeleton DW_AT_high_pc has an unsupported value class")}
		}
		if v, ok := e.Val(dwarf.AttrAddrBase).(int64); ok {
			s.addrBase = uint64(v)
		} else {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(s.rootDIEOff), Op: "parse skeleton DIEs", DWOID: s.dwoID,
				Err: fmt.Errorf("skeleton lacks DW_AT_addr_base")}
		}
		if v, ok := e.Val(dwarf.AttrCompDir).(string); ok {
			s.compDir = v
		}
		if v, ok := e.Val(dwarf.AttrDwoName).(string); ok {
			s.dwoName = v
		}
		block, ok := locateAddrBlock(imgAddr, addrSec, s.addrBase)
		if !ok {
			return nil, &SplitError{Object: "image", Section: ".debug_addr",
				Offset: int64(s.addrBase), FileOffset: int64(addrSec.Offset + s.addrBase),
				Op: "resolve addr_base", DWOID: s.dwoID,
				Err: fmt.Errorf("DW_AT_addr_base %#x does not point at an address block", s.addrBase)}
		}
		s.addr = block
	}

	seenID := map[uint64]bool{}
	var functions []SplitFunction
	for _, s := range skels {
		if seenID[s.dwoID] {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(s.headerOff), FileOffset: int64(infoSec.Offset + s.headerOff),
				Op: "pair skeleton", DWOID: s.dwoID,
				Err: fmt.Errorf("duplicate DWO identity among skeleton units")}
		}
		seenID[s.dwoID] = true

		result := pkg.Lookup(s.dwoID)
		if !result.Found() {
			return nil, &SplitError{Object: "dwp", Section: ".debug_cu_index", Op: "lookup DWO id",
				DWOID: s.dwoID,
				Err:   fmt.Errorf("no package unit for skeleton DWO id %#016x (probe ended: %s)", s.dwoID, missText(result.Miss))}
		}
		fns, err := pkg.recoverUnit(result.Unit, &s, imgAddr, addrSec, infoSec)
		if err != nil {
			return nil, err
		}
		functions = append(functions, fns...)
	}
	return functions, nil
}

// SelectSplitFunctions returns every record whose ranges contain pc. Ranges
// are half-open: a pc equal to a range low bound matches, a pc equal to the
// high bound does not. The returned slice is empty (never nil for a
// successful call with no match) when no record contains pc; no nearest
// function is guessed.
func SelectSplitFunctions(functions []SplitFunction, pc uint64) []SplitFunction {
	matches := []SplitFunction{}
	for i := range functions {
		fn := &functions[i]
		for _, r := range fn.Ranges {
			if r.Low <= pc && pc < r.High {
				matches = append(matches, *fn)
				break
			}
		}
	}
	return matches
}

// checkSplitInput enforces the supported container envelope.
func (f *File) checkSplitInput() error {
	if f.Class != ELFCLASS64 {
		return &SplitError{Object: "image", Op: "open", Err: errUnsupported("only ELF64 images are supported, got %v", f.Class)}
	}
	if f.Data != ELFDATA2LSB {
		return &SplitError{Object: "image", Op: "open", Err: errUnsupported("only little-endian images are supported, got %v", f.Data)}
	}
	for _, s := range f.Sections {
		if s.Flags&SHF_COMPRESSED != 0 {
			return &SplitError{Object: "image", Section: s.Name, Offset: int64(s.Offset), Op: "open",
				Err: errUnsupported("compressed section %q is not supported", s.Name)}
		}
		if s.Type == SHT_REL || s.Type == SHT_RELA {
			return &SplitError{Object: "image", Section: s.Name, Offset: int64(s.Offset), Op: "open",
				Err: errUnsupported("relocations are not supported")}
		}
	}
	for _, name := range []string{".debug_info", ".debug_abbrev", ".debug_str", ".debug_str_offsets", ".debug_addr"} {
		if f.Section(name) == nil {
			return &SplitError{Object: "image", Section: name, Op: "open",
				Err: fmt.Errorf("required section %q is absent", name)}
		}
	}
	return nil
}

// readOwnedSection returns a private copy of a section's bytes.
func readOwnedSection(f *File, name string) ([]byte, error) {
	s := f.Section(name)
	if s == nil {
		return nil, &SplitError{Object: "image", Section: name, Op: "read", Err: fmt.Errorf("section is absent")}
	}
	b, err := cloneSection(s)
	if err != nil {
		return nil, &SplitError{Object: "image", Section: name, Offset: int64(s.Offset), Op: "read", Err: err}
	}
	return b, nil
}

// parseSkeletons walks the image .debug_info using only the fixed DWARF32
// version 5 skeleton header layout, retaining every original offset.
func parseSkeletons(bo binary.ByteOrder, info, abbrev []byte, infoSec *Section) ([]skeletonUnit, error) {
	var out []skeletonUnit
	off := uint64(0)
	for off < uint64(len(info)) {
		if uint64(len(info))-off < 20 {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(off), FileOffset: int64(infoSec.Offset + off),
				Op: "parse skeleton header", Err: fmt.Errorf("truncated unit header")}
		}
		hdr := off
		unitLength := uint64(bo.Uint32(info[off:]))
		if unitLength >= uint64(dwarf32MaxLength) {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(off), FileOffset: int64(infoSec.Offset + off),
				Op:  "parse skeleton header",
				Err: errUnsupported("DWARF64 unit length %#x", unitLength)}
		}
		end := off + 4 + unitLength
		if end > uint64(len(info)) {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(off), FileOffset: int64(infoSec.Offset + off),
				Op: "parse skeleton header", Err: fmt.Errorf("unit length %#x runs past section", unitLength)}
		}
		version := bo.Uint16(info[off+4:])
		unitType := info[off+6]
		addrSize := info[off+7]
		abbrevOff := uint64(bo.Uint32(info[off+8:]))
		dwoID := bo.Uint64(info[off+12:])
		if version != 5 {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(off), FileOffset: int64(infoSec.Offset + off),
				Op: "parse skeleton header", DWOID: dwoID,
				Err: errUnsupported("DWARF version %d (only version 5 is supported)", version)}
		}
		if unitType != utSkeleton {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(off), FileOffset: int64(infoSec.Offset + off),
				Op: "parse skeleton header", DWOID: dwoID,
				Err: errUnsupported("unit type %#x (only DW_UT_skeleton %#x is supported in the image)", unitType, utSkeleton)}
		}
		if addrSize != 8 {
			return nil, &SplitError{Object: "image", Section: ".debug_info",
				Offset: int64(off), FileOffset: int64(infoSec.Offset + off),
				Op: "parse skeleton header", DWOID: dwoID,
				Err: errUnsupported("address size %d (only 8 is supported)", addrSize)}
		}
		if abbrevOff >= uint64(len(abbrev)) {
			return nil, &SplitError{Object: "image", Section: ".debug_abbrev",
				Offset: int64(abbrevOff), Op: "parse skeleton header", DWOID: dwoID,
				Err: fmt.Errorf("skeleton abbrev_offset %#x is outside .debug_abbrev of size %#x",
					abbrevOff, len(abbrev))}
		}
		out = append(out, skeletonUnit{
			dwoID:      dwoID,
			headerOff:  hdr,
			length:     unitLength,
			rootDIEOff: hdr + 20,
			abbrevOff:  abbrevOff,
		})
		off = end
	}
	return out, nil
}

// checkAddrBlocks validates the global shape of .debug_addr before any
// skeleton's addr_base is resolved against it.
func checkAddrBlocks(addr []byte, addrSec *Section) error {
	bo := binary.LittleEndian
	off := uint64(0)
	for off+8 <= uint64(len(addr)) {
		length := uint64(bo.Uint32(addr[off:]))
		if length < 4 || off+4+length > uint64(len(addr)) {
			return &SplitError{Object: "image", Section: ".debug_addr",
				Offset: int64(off), FileOffset: int64(addrSec.Offset + off),
				Op: "parse .debug_addr", Err: fmt.Errorf("invalid block length %#x", length)}
		}
		if bo.Uint16(addr[off+4:]) != 5 || addr[off+6] != 8 {
			return &SplitError{Object: "image", Section: ".debug_addr",
				Offset: int64(off), FileOffset: int64(addrSec.Offset + off),
				Op: "parse .debug_addr", Err: errUnsupported("only version 5, 8-byte address blocks are supported")}
		}
		off += 4 + length
	}
	if off != uint64(len(addr)) {
		return &SplitError{Object: "image", Section: ".debug_addr",
			Offset: int64(off), FileOffset: int64(addrSec.Offset + off),
			Op: "parse .debug_addr", Err: fmt.Errorf("trailing %d bytes after address blocks", uint64(len(addr))-off)}
	}
	return nil
}

// locateAddrBlock finds the .debug_addr block whose entry region starts at
// base, by walking the sequence of per-unit block headers in the section.
func locateAddrBlock(addr []byte, addrSec *Section, base uint64) (addrBlock, bool) {
	bo := binary.LittleEndian
	off := uint64(0)
	for off+8 <= uint64(len(addr)) {
		length := uint64(bo.Uint32(addr[off:]))
		if length < 4 || off+4+length > uint64(len(addr)) {
			return addrBlock{}, false
		}
		entryBase := off + 8
		blockEnd := off + 4 + length
		if entryBase == base {
			return addrBlock{base: entryBase, end: blockEnd, fileStart: addrSec.Offset + off}, true
		}
		off = blockEnd
	}
	return addrBlock{}, false
}

// recoverUnit validates one split compile unit against its skeleton and
// returns the function records contained in it.
func (p *DWARFPackage) recoverUnit(unit *DWPUnit, skel *skeletonUnit,
	imgAddr []byte, addrSec, infoSec *Section) ([]SplitFunction, error) {

	infoContrib, ok := unit.Contributions[dwpSecInfo]
	if !ok || infoContrib.Size == 0 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_info.dwo", Op: "recover unit",
			DWOID: skel.dwoID, Err: fmt.Errorf("index row %d lacks an INFO contribution", unit.IndexRow)}
	}
	abbrevContrib, ok := unit.Contributions[dwpSecAbbrev]
	if !ok || abbrevContrib.Size == 0 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_abbrev.dwo", Op: "recover unit",
			DWOID: skel.dwoID, Err: fmt.Errorf("index row %d lacks an ABBREV contribution", unit.IndexRow)}
	}
	strOffContrib, ok := unit.Contributions[dwpSecStrOffsets]
	if !ok || strOffContrib.Size == 0 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_str_offsets.dwo", Op: "recover unit",
			DWOID: skel.dwoID, Err: fmt.Errorf("index row %d lacks a STR_OFFSETS contribution", unit.IndexRow)}
	}
	rngContrib, rngListed := unit.Contributions[dwpSecRnglists]
	hasRng := rngListed && rngContrib.Size > 0

	infoAll, _ := p.sectionBytes(".debug_info.dwo")
	abbrevAll, _ := p.sectionBytes(".debug_abbrev.dwo")
	strOffAll, _ := p.sectionBytes(".debug_str_offsets.dwo")
	rngAll, _ := p.sectionBytes(".debug_rnglists.dwo")
	strAll, _ := p.sectionBytes(".debug_str.dwo")

	infoView := infoAll[infoContrib.Offset : infoContrib.Offset+infoContrib.Size]
	if err := checkSplitHeader(p.byteOrder, infoView, skel.dwoID, abbrevContrib.Size); err != nil {
		return nil, &SplitError{Object: "dwp", Section: ".debug_info.dwo",
			Offset: int64(infoContrib.Offset), FileOffset: int64(infoContrib.FileOffset),
			Op: "parse split header", DWOID: skel.dwoID, Err: err}
	}
	abbrevView := abbrevAll[abbrevContrib.Offset : abbrevContrib.Offset+abbrevContrib.Size]
	tables, err := parseAbbrevTables(abbrevView)
	if err != nil {
		return nil, &SplitError{Object: "dwp", Section: ".debug_abbrev.dwo",
			Offset: int64(abbrevContrib.Offset), FileOffset: int64(abbrevContrib.FileOffset),
			Op: "parse abbrev table", DWOID: skel.dwoID, Err: err}
	}

	// The str_offsets contribution carries its own 8-byte DWARF32 header; the
	// standard library treats the section as a flat array, so the owned view
	// starts at the first entry, which is also the split unit's implicit
	// string offset base.
	strOffView, err := strOffsetEntryView(strOffAll, strOffContrib)
	if err != nil {
		return nil, &SplitError{Object: "dwp", Section: ".debug_str_offsets.dwo",
			Offset: int64(strOffContrib.Offset), FileOffset: int64(strOffContrib.FileOffset),
			Op: "parse str_offsets header", DWOID: skel.dwoID, Err: err}
	}

	// The split unit has no addr table of its own: its addrx indexes resolve
	// through the paired skeleton's addr_base inside the image.
	if skel.addrBase+8 > skel.addr.end {
		return nil, &SplitError{Object: "image", Section: ".debug_addr",
			Offset: int64(skel.addrBase), FileOffset: int64(addrSec.Offset + skel.addrBase),
			Op: "resolve addr_base", DWOID: skel.dwoID,
			Err: fmt.Errorf("skeleton address contribution has no entries")}
	}
	addrView := imgAddr[skel.addrBase:skel.addr.end]

	d, err := dwarf.New(abbrevView, nil, nil, infoView, nil, nil, nil, strAll)
	if err != nil {
		return nil, &SplitError{Object: "dwp", Section: ".debug_info.dwo",
			Offset: int64(infoContrib.Offset), FileOffset: int64(infoContrib.FileOffset),
			Op: "parse split DIEs", DWOID: skel.dwoID, Err: err}
	}
	if err := d.AddSection(".debug_str_offsets", strOffView); err != nil {
		return nil, &SplitError{Object: "dwp", Section: ".debug_str_offsets.dwo",
			Offset: int64(strOffContrib.Offset), FileOffset: int64(strOffContrib.FileOffset),
			Op: "parse split DIEs", DWOID: skel.dwoID, Err: err}
	}
	if err := d.AddSection(".debug_addr", addrView); err != nil {
		return nil, &SplitError{Object: "image", Section: ".debug_addr",
			Offset: int64(skel.addrBase), FileOffset: int64(addrSec.Offset + skel.addrBase),
			Op: "parse split DIEs", DWOID: skel.dwoID, Err: err}
	}
	if hasRng && rngContrib.Size > 0 {
		if err := d.AddSection(".debug_rnglists", rngAll[rngContrib.Offset:rngContrib.Offset+rngContrib.Size]); err != nil {
			return nil, &SplitError{Object: "dwp", Section: ".debug_rnglists.dwo",
				Offset: int64(rngContrib.Offset), FileOffset: int64(rngContrib.FileOffset),
				Op: "parse split DIEs", DWOID: skel.dwoID, Err: err}
		}
	}

	var rootName string
	var functions []SplitFunction
	reader := d.Reader()
	for {
		e, err := reader.Next()
		if err != nil {
			return nil, &SplitError{Object: "dwp", Section: ".debug_info.dwo",
				Offset:     int64(infoContrib.Offset + uint64(e.Offset)),
				FileOffset: int64(infoContrib.FileOffset + uint64(e.Offset)),
				Op:         "parse split DIEs", DWOID: skel.dwoID, Err: err}
		}
		if e == nil {
			break
		}
		if e.Tag == dwarf.TagCompileUnit {
			if v, ok := e.Val(dwarf.AttrName).(string); ok {
				rootName = v
			}
			continue
		}
		if e.Tag != dwarf.TagSubprogram {
			continue
		}
		fn, used, err := p.buildFunction(skel, e, infoView, tables, infoContrib,
			abbrevContrib, strOffContrib, rngContrib, hasRng, rngAll,
			imgAddr, addrSec, infoSec)
		if err != nil {
			return nil, err
		}
		if !used {
			continue // declaration without code
		}
		fn.Name, _ = e.Val(dwarf.AttrName).(string)
		fn.CompileUnitName = rootName
		fn.SkeletonCompDir = skel.compDir
		fn.DWOID = skel.dwoID
		fn.IndexRow = unit.IndexRow
		functions = append(functions, fn)
	}
	if len(functions) == 0 {
		return nil, &SplitError{Object: "dwp", Section: ".debug_info.dwo",
			Offset: int64(infoContrib.Offset), FileOffset: int64(infoContrib.FileOffset),
			Op: "recover unit", DWOID: skel.dwoID,
			Err: fmt.Errorf("split unit contains no subprogram with code ranges")}
	}
	return functions, nil
}

// buildFunction turns one subprogram entry into a SplitFunction, decoding the
// raw forms itself so that addrx/rnglistx evidence positions refer to the
// original bytes rather than to the standard library's re-based buffers.
func (p *DWARFPackage) buildFunction(skel *skeletonUnit, e *dwarf.Entry,
	infoView []byte, tables abbrevTableSet, infoContrib, abbrevContrib, strOffContrib *DWPContribution,
	rngContrib *DWPContribution, hasRng bool, rngAll []byte,
	imgAddr []byte, addrSec, infoSec *Section) (SplitFunction, bool, error) {

	var fn SplitFunction
	localOff := uint64(e.Offset)
	scanner, err := newFormScanner(infoView, localOff, tables)
	if err != nil {
		return fn, false, splitDWPErr(infoContrib, skel.dwoID, "scan function DIE", err)
	}

	var (
		hasLowPC  bool
		hasRanges bool
		lowPC     uint64
		highPC    uint64
		highConst bool
	)
	for scanner.next() {
		spec := scanner.attr
		switch spec.attr {
		case uint32(dwarf.AttrLowpc):
			hasLowPC = true
			switch spec.form {
			case formAddr:
				lowPC = scanner.uvalue
			case formAddrx, formAddrx1, formAddrx2, formAddrx3, formAddrx4:
				idx := scanner.uvalue
				entrySecOff, entryFileOff, val, err := resolveAddrx(imgAddr, addrSec, skel, idx)
				if err != nil {
					return fn, false, &SplitError{Object: "image", Section: ".debug_addr",
						Offset: int64(entrySecOff), FileOffset: int64(entryFileOff),
						Op: "resolve addrx", DWOID: skel.dwoID, Err: err}
				}
				lowPC = val
				fn.Addr = &SplitAddrEvidence{
					Index: idx,
					Base:  skel.addrBase,
					TableEntry: SplitPosition{
						Object:        "image",
						Section:       ".debug_addr",
						SectionOffset: entrySecOff,
						FileOffset:    entryFileOff,
					},
					Value: val,
				}
			default:
				return fn, false, splitDWPErr(infoContrib, skel.dwoID, "resolve DW_AT_low_pc",
					errUnsupported("DW_AT_low_pc form %#x", spec.form))
			}
		case uint32(dwarf.AttrHighpc):
			switch spec.form {
			case formAddr:
				highPC = scanner.uvalue
				highConst = false
			case formData1, formData2, formData4, formData8, formUData:
				highPC = scanner.uvalue
				highConst = true
			case formSData:
				highPC = uint64(int64(scanner.svalue))
				highConst = true
			default:
				return fn, false, splitDWPErr(infoContrib, skel.dwoID, "resolve DW_AT_high_pc",
					errUnsupported("DW_AT_high_pc form %#x", spec.form))
			}
		case uint32(dwarf.AttrRanges):
			hasRanges = true
			if spec.form != formRnglistx {
				return fn, false, splitDWPErr(infoContrib, skel.dwoID, "resolve DW_AT_ranges",
					errUnsupported("DW_AT_ranges form %#x (only DW_FORM_rnglistx is supported)", spec.form))
			}
		}
	}
	if scanner.err != nil {
		return fn, false, splitDWPErr(infoContrib, skel.dwoID, "scan function DIE", scanner.err)
	}

	// Declarations without any code attribution are ignored.
	if !hasLowPC && !hasRanges {
		return fn, false, nil
	}

	// Positions: local, section-relative and absolute package offsets all
	// describe the same original DIE bytes.
	sectionOff := infoContrib.Offset + localOff
	fileOff := infoContrib.FileOffset + localOff
	fn.FunctionDIELocal = SplitPosition{Object: "dwp", Section: ".debug_info.dwo",
		SectionOffset: localOff, FileOffset: localOff}
	fn.FunctionDIESection = SplitPosition{Object: "dwp", Section: ".debug_info.dwo",
		SectionOffset: sectionOff, FileOffset: fileOff}
	fn.FunctionDIEFile = SplitPosition{Object: "dwp", Section: ".debug_info.dwo",
		SectionOffset: sectionOff, FileOffset: fileOff}

	fn.SkeletonHeader = SplitPosition{Object: "image", Section: ".debug_info",
		SectionOffset: skel.headerOff, FileOffset: infoSec.Offset + skel.headerOff}
	fn.SkeletonRootDIE = SplitPosition{Object: "image", Section: ".debug_info",
		SectionOffset: skel.rootDIEOff, FileOffset: infoSec.Offset + skel.rootDIEOff}

	fn.Contributions = map[uint32]DWPContribution{
		dwpSecInfo:       *infoContrib,
		dwpSecAbbrev:     *abbrevContrib,
		dwpSecStrOffsets: *strOffContrib,
	}
	if hasRng {
		fn.Contributions[dwpSecRnglists] = *rngContrib
	} else {
		// Explicit, zeroed marker: this unit made no rnglists contribution.
		fn.Contributions[dwpSecRnglists] = DWPContribution{
			Section: dwpSecRnglists, SectionName: ".debug_rnglists.dwo",
		}
	}

	switch {
	case hasRanges:
		idx, idxPos, listPos, ranges, err := decodeRnglistx(infoView, localOff, tables,
			rngAll, rngContrib, skel)
		if err != nil {
			return fn, false, err
		}
		fn.Ranges = ranges
		fn.Rnglist = &SplitRnglistEvidence{
			Index:            idx,
			OffsetTableEntry: idxPos,
			List:             listPos,
			BasePC:           skel.lowPC,
		}
	case hasLowPC:
		high := highPC
		if highConst {
			high = lowPC + highPC
		}
		if high <= lowPC {
			return fn, false, splitDWPErr(infoContrib, skel.dwoID, "resolve ranges",
				fmt.Errorf("empty or inverted range [%#x, %#x)", lowPC, high))
		}
		fn.Ranges = []SplitRange{{Low: lowPC, High: high}}
		// Cross-check the standard library's own addrx resolution against the
		// raw evidence before trusting either one.
		if gv, ok := e.Val(dwarf.AttrLowpc).(uint64); ok && gv != lowPC {
			return fn, false, splitDWPErr(infoContrib, skel.dwoID, "cross-check addrx",
				fmt.Errorf("library resolved low_pc %#x but raw evidence gives %#x", gv, lowPC))
		}
	default:
		return fn, false, splitDWPErr(infoContrib, skel.dwoID, "resolve ranges",
			fmt.Errorf("subprogram has neither DW_AT_low_pc nor DW_AT_ranges"))
	}
	return fn, true, nil
}

// resolveAddrx reads address index idx from the skeleton's own .debug_addr
// contribution, rejecting references that leave that contribution.
func resolveAddrx(addr []byte, sec *Section, skel *skeletonUnit, idx uint64) (secOff, fileOff, value uint64, err error) {
	entryOff := skel.addrBase + idx*8
	if entryOff+8 > skel.addr.end {
		return entryOff, sec.Offset + entryOff, 0,
			fmt.Errorf("addrx index %d reaches %#x, outside skeleton address contribution ending %#x",
				idx, entryOff+8, skel.addr.end)
	}
	value = binary.LittleEndian.Uint64(addr[entryOff:])
	return entryOff, sec.Offset + entryOff, value, nil
}

// decodeRnglistx manually resolves an rnglistx reference and decodes the
// referenced list, supporting only offset_pair and end_of_list.
func decodeRnglistx(infoView []byte, localOff uint64, tables abbrevTableSet,
	rngAll []byte, contrib *DWPContribution, skel *skeletonUnit) (
	idx uint64, idxPos, listPos SplitPosition, ranges []SplitRange, err error) {

	scanner, serr := newFormScanner(infoView, localOff, tables)
	if serr != nil {
		return 0, SplitPosition{}, SplitPosition{}, nil, serr
	}
	var found bool
	for scanner.next() {
		if scanner.attr.attr == uint32(dwarf.AttrRanges) && scanner.attr.form == formRnglistx {
			idx = scanner.uvalue
			found = true
			break
		}
	}
	if scanner.err != nil {
		return 0, SplitPosition{}, SplitPosition{}, nil, scanner.err
	}
	if !found {
		return 0, SplitPosition{}, SplitPosition{}, nil, fmt.Errorf("rnglistx attribute vanished during rescan")
	}
	if !contribPresent(contrib) {
		return 0, SplitPosition{}, SplitPosition{}, nil, &SplitError{
			Object: "dwp", Section: ".debug_rnglists.dwo", Op: "resolve rnglistx", DWOID: skel.dwoID,
			Err: fmt.Errorf("unit index row has no RNGLISTS contribution but a function uses rnglistx")}
	}
	rb := rngAll[contrib.Offset : contrib.Offset+contrib.Size]
	bo := binary.LittleEndian
	if len(rb) < 12 {
		return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, 0, "truncated rnglists header")
	}
	unitLength := uint64(bo.Uint32(rb[0:]))
	if unitLength >= uint64(dwarf32MaxLength) {
		return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, 0, "DWARF64 rnglists are not supported")
	}
	version := bo.Uint16(rb[4:])
	addrSize := rb[6]
	if version != 5 || addrSize != 8 {
		return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, 0,
			fmt.Sprintf("unsupported rnglists header version=%d addr_size=%d", version, addrSize))
	}
	offsetCount := uint64(bo.Uint32(rb[8:]))
	tableStart := uint64(12)
	tableEnd := tableStart + offsetCount*4
	if tableEnd > 4+unitLength || tableEnd > uint64(len(rb)) {
		return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, 8, "rnglists offset table exceeds contribution")
	}
	if idx >= offsetCount {
		return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID,
			tableStart+idx*4, fmt.Sprintf("rnglistx index %d exceeds offset_count %d", idx, offsetCount))
	}
	entryLocal := tableStart + idx*4
	entryValue := uint64(bo.Uint32(rb[entryLocal:]))
	listLocal := tableStart + entryValue
	if listLocal < tableEnd || listLocal >= 4+unitLength {
		return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID,
			entryLocal, fmt.Sprintf("offset table entry %#x points outside the list area", entryValue))
	}
	idxPos = SplitPosition{
		Object: "dwp", Section: ".debug_rnglists.dwo",
		SectionOffset: contrib.Offset + entryLocal,
		FileOffset:    contrib.FileOffset + entryLocal,
	}
	listPos = SplitPosition{
		Object: "dwp", Section: ".debug_rnglists.dwo",
		SectionOffset: contrib.Offset + listLocal,
		FileOffset:    contrib.FileOffset + listLocal,
	}

	p := listLocal
	limit := 4 + unitLength
	for p < limit {
		op := rb[p]
		if op == rleEndOfList {
			break
		}
		if op != rleOffsetPair {
			return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, p,
				errUnsupported("rnglist opcode %#x (only offset_pair and end_of_list are supported)", op).Error())
		}
		a, q := readULEB128(rb, int(p)+1)
		if q <= int(p) || uint64(q) >= limit {
			return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, p, "truncated offset_pair")
		}
		b, q2 := readULEB128(rb, q)
		if uint64(q2) > limit {
			return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, p, "truncated offset_pair")
		}
		low := skel.lowPC + a
		high := skel.lowPC + b
		if high <= low {
			return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, p,
				fmt.Sprintf("empty or inverted offset_pair [%#x, %#x)", a, b))
		}
		ranges = append(ranges, SplitRange{Low: low, High: high})
		p = uint64(q2)
	}
	if len(ranges) == 0 {
		return 0, SplitPosition{}, SplitPosition{}, nil, splitRngErr(contrib, skel.dwoID, listLocal,
			"rnglist contains no offset_pair entries")
	}
	return idx, idxPos, listPos, ranges, nil
}

func contribPresent(c *DWPContribution) bool {
	return c != nil && c.Size > 0
}

func splitDWPErr(c *DWPContribution, dwo uint64, op string, err error) *SplitError {
	return &SplitError{
		Object: "dwp", Section: c.SectionName,
		Offset: int64(c.Offset), FileOffset: int64(c.FileOffset),
		Op: op, DWOID: dwo, Err: err,
	}
}

func splitRngErr(c *DWPContribution, dwo uint64, localOff uint64, msg string) *SplitError {
	return &SplitError{
		Object: "dwp", Section: ".debug_rnglists.dwo",
		Offset: int64(c.Offset + localOff), FileOffset: int64(c.FileOffset + localOff),
		Op: "resolve rnglistx", DWOID: dwo, Err: fmt.Errorf("%s", msg),
	}
}

func missText(m *DWPIndexMiss) string {
	if m == nil {
		return "no details"
	}
	switch m.Reason {
	case DWPMissEmptySlot:
		return fmt.Sprintf("empty slot at bucket %d after %d probe(s)", m.TerminatingBucket, m.Probes)
	case DWPMissTableFull:
		return fmt.Sprintf("table full after %d probes", m.Probes)
	default:
		return "unknown reason"
	}
}

// checkSplitHeader validates the fixed DWARF32 version 5 split compile header
// and confirms its embedded DWO identifier matches the skeleton.
func checkSplitHeader(bo binary.ByteOrder, view []byte, wantID uint64, abbrevContribSize uint64) error {
	if len(view) < 20 {
		return fmt.Errorf("truncated split unit header")
	}
	unitLength := uint64(bo.Uint32(view[0:]))
	if unitLength >= uint64(dwarf32MaxLength) {
		return errUnsupported("DWARF64 split unit length %#x", unitLength)
	}
	if 4+unitLength > uint64(len(view)) {
		return fmt.Errorf("split unit length %#x exceeds contribution %#x", unitLength, len(view))
	}
	if bo.Uint16(view[4:]) != 5 {
		return errUnsupported("split unit is not DWARF version 5")
	}
	if view[6] != utSplitCompile {
		return errUnsupported("unit type %#x (only DW_UT_split_compile %#x is supported in the package)", view[6], utSplitCompile)
	}
	if view[7] != 8 {
		return errUnsupported("split unit address size %d (only 8 is supported)", view[7])
	}
	// The on-disk DWARF32 split compile header produced by the toolchain is
	// unit_length(4) version(2) unit_type(1) address_size(1)
	// debug_abbrev_offset(4) dwo_id(8): the abbreviation offset comes first
	// and the root DIE begins at byte 20.
	abbrevOff := uint64(bo.Uint32(view[8:]))
	if got := bo.Uint64(view[12:]); got != wantID {
		return fmt.Errorf("split header DWO id %#016x does not match skeleton id %#016x", got, wantID)
	}
	if abbrevOff >= abbrevContribSize {
		return fmt.Errorf("abbrev_offset %#x is outside the unit's ABBREV contribution of size %#x",
			abbrevOff, abbrevContribSize)
	}
	return nil
}

// strOffsetEntryView validates a str_offsets contribution header and returns a
// flat view beginning at its first entry, which is the split unit's implicit
// string offset base.
func strOffsetEntryView(all []byte, c *DWPContribution) ([]byte, error) {
	b := all[c.Offset : c.Offset+c.Size]
	if len(b) < 8 {
		return nil, fmt.Errorf("truncated str_offsets contribution")
	}
	bo := binary.LittleEndian
	unitLength := uint64(bo.Uint32(b[0:]))
	if unitLength >= uint64(dwarf32MaxLength) {
		return nil, errUnsupported("DWARF64 str_offsets length %#x", unitLength)
	}
	if bo.Uint16(b[4:]) != 5 {
		return nil, fmt.Errorf("str_offsets contribution is not version 5")
	}
	if 4+unitLength > uint64(len(b)) {
		return nil, fmt.Errorf("str_offsets length %#x exceeds contribution", unitLength)
	}
	return b[8:], nil
}

// abbrevAttrSpec is one (attribute, form) pair of an abbreviation.
type abbrevAttrSpec struct {
	attr uint32
	form uint32
}

// abbrevTableSet maps abbreviation codes within a contribution to specs.
type abbrevTableSet map[uint64][]abbrevAttrSpec

// parseAbbrevTables decodes a full .debug_abbrev.dwo contribution. It only
// needs attribute/form pairs; the standard library remains the DIE decoder.
func parseAbbrevTables(b []byte) (abbrevTableSet, error) {
	tables := abbrevTableSet{}
	p := 0
	for p < len(b) {
		code, q := readULEB128(b, p)
		p = q
		if code == 0 {
			break
		}
		_, q = readULEB128(b, p) // tag
		p = q
		if p >= len(b) {
			return nil, fmt.Errorf("truncated abbreviation %d", code)
		}
		p++ // has_children
		var specs []abbrevAttrSpec
		for {
			attr, q2 := readULEB128(b, p)
			p = q2
			form, q3 := readULEB128(b, p)
			p = q3
			if attr == 0 && form == 0 {
				break
			}
			specs = append(specs, abbrevAttrSpec{uint32(attr), uint32(form)})
		}
		if _, dup := tables[code]; dup {
			return nil, fmt.Errorf("duplicate abbreviation code %d", code)
		}
		tables[code] = specs
	}
	return tables, nil
}

// formScanner walks the attribute bytes of one DIE using an abbreviation
// table, recording the raw byte span of every value without turning the
// process into a second full DIE decoder.
type formScanner struct {
	b         []byte
	pos       int
	specs     []abbrevAttrSpec
	specIndex int
	attr      abbrevAttrSpec
	uvalue    uint64
	svalue    int64
	err       error
}

func newFormScanner(info []byte, dieOff uint64, tables abbrevTableSet) (*formScanner, error) {
	code, p := readULEB128(info, int(dieOff))
	specs, ok := tables[code]
	if !ok {
		return nil, fmt.Errorf("abbreviation code %d at DIE offset %#x is not defined", code, dieOff)
	}
	return &formScanner{b: info, pos: p, specs: specs}, nil
}

func (s *formScanner) next() bool {
	if s.err != nil || s.specIndex >= len(s.specs) {
		return false
	}
	s.attr = s.specs[s.specIndex]
	s.specIndex++
	v, sv, np, err := skipForm(s.b, s.pos, s.attr.form)
	if err != nil {
		s.err = err
		return false
	}
	s.uvalue = v
	s.svalue = sv
	s.pos = np
	return true
}

// skipForm consumes one form value, returning its unsigned and signed
// interpretations for the constant/address/index forms used by the chain.
func skipForm(b []byte, pos int, form uint32) (uint64, int64, int, error) {
	if pos >= len(b) {
		return 0, 0, pos, fmt.Errorf("value of form %#x starts past section end", form)
	}
	bo := binary.LittleEndian
	switch form {
	case formFlagPresent, formImplicitConst:
		// flag_present carries no bytes. An implicit_const value lives in the
		// abbreviation; no supported input needs it for range recovery.
		return 0, 0, pos, nil
	case formData1, formAddrx1, formStrx1:
		return uint64(b[pos]), int64(b[pos]), pos + 1, nil
	case formData2, formAddrx2, formStrx2:
		if pos+2 > len(b) {
			return 0, 0, pos, fmt.Errorf("truncated 2-byte form %#x", form)
		}
		v := bo.Uint16(b[pos:])
		return uint64(v), int64(int16(v)), pos + 2, nil
	case formData4, formAddrx4, formStrx4, formRef4, formSecOffset:
		if pos+4 > len(b) {
			return 0, 0, pos, fmt.Errorf("truncated 4-byte form %#x", form)
		}
		v := bo.Uint32(b[pos:])
		return uint64(v), int64(int32(v)), pos + 4, nil
	case formData8, formAddr:
		if pos+8 > len(b) {
			return 0, 0, pos, fmt.Errorf("truncated 8-byte form %#x", form)
		}
		v := bo.Uint64(b[pos:])
		return v, int64(v), pos + 8, nil
	case formAddrx3, formStrx3:
		if pos+3 > len(b) {
			return 0, 0, pos, fmt.Errorf("truncated 3-byte form %#x", form)
		}
		v := uint64(b[pos]) | uint64(b[pos+1])<<8 | uint64(b[pos+2])<<16
		return v, int64(int32(v) << 8 >> 8), pos + 3, nil
	case formUData, formAddrx, formStrx, formRnglistx:
		v, np := readULEB128(b, pos)
		if np == pos {
			return 0, 0, pos, fmt.Errorf("truncated ULEB form %#x", form)
		}
		return v, int64(v), np, nil
	case formSData:
		v, np := readSLEB128(b, pos)
		if np == pos {
			return 0, 0, pos, fmt.Errorf("truncated SLEB form %#x", form)
		}
		return uint64(v), v, np, nil
	case formExprLoc:
		n, np := readULEB128(b, pos)
		if np+int(n) > len(b) {
			return 0, 0, pos, fmt.Errorf("truncated exprloc block")
		}
		return 0, 0, np + int(n), nil
	default:
		// String/block/reference forms are not needed to recover ranges; the
		// supported inputs never place them inside range-bearing attributes.
		return 0, 0, pos, errUnsupported("attribute form %#x while scanning function DIE", form)
	}
}

func readULEB128(b []byte, pos int) (uint64, int) {
	var v uint64
	var shift uint
	for pos < len(b) {
		c := b[pos]
		pos++
		v |= uint64(c&0x7f) << shift
		if c&0x80 == 0 {
			return v, pos
		}
		shift += 7
		if shift >= 64 {
			return 0, pos
		}
	}
	return 0, pos
}

func readSLEB128(b []byte, pos int) (int64, int) {
	var v int64
	var shift uint
	for pos < len(b) {
		c := b[pos]
		pos++
		v |= int64(c&0x7f) << shift
		shift += 7
		if c&0x80 == 0 {
			if c&0x40 != 0 && shift < 64 {
				v |= -(1 << shift)
			}
			return v, pos
		}
		if shift >= 64 {
			return 0, pos
		}
	}
	return 0, pos
}
