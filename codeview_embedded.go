package pe

import (
	"encoding/binary"
	"math"
	"sort"

	"github.com/pkg/errors"
)

// OMF CodeView subsection types and constants.
//
// ref: Wine include/wine/mscvpdb.h
const (
	cvSstModule    uint16 = 0x120 // module descriptor
	cvSstSrcModule uint16 = 0x127 // source module (files and line tables)
	cvSstSegMap    uint16 = 0x12d // global logical-to-physical segment map

	cvSegMapFlatFlags uint16 = 0x0109 // flat 32-bit segment descriptor
	cvSegMapGlobalMod uint16 = 0xffff // sstSegMap directory iMod
)

// OMFDirectoryEntry is one raw entry of the OMF directory. All offsets are
// relative to the start of the CodeView payload.
type OMFDirectoryEntry struct {
	// Subsection is the OMF subsection type (e.g. sstModule, sstSrcModule).
	Subsection uint16
	// Module is the iMod the subsection belongs to; 0xffff for sstSegMap.
	Module uint16
	// Offset is the payload-relative offset of the subsection.
	Offset uint32
	// Size is the subsection size in bytes.
	Size uint32
}

// OMFSegment is one entry of the global sstSegMap table. The array position
// (plus one) is the logical segment number.
type OMFSegment struct {
	Flags      uint16
	Overlay    uint16
	Group      uint16
	Frame      uint16 // 1-based physical PE section number
	ISegName   uint16
	IClassName uint16
	// Offset is the logical-segment base offset inside the physical PE section.
	Offset uint32
	// Size is the logical segment size in bytes.
	Size uint32
}

// OMFContribution is one segment contribution of a module. Offsets are local
// to the logical segment identified by Seg.
type OMFContribution struct {
	// Seg is the logical segment number.
	Seg uint16
	// Off is the contribution start offset within the logical segment.
	Off uint32
	// Size is the contribution size; the range is [Off, Off+Size).
	Size uint32
}

// EmbeddedModule describes one OMF module (sstModule).
type EmbeddedModule struct {
	// Module is the iMod identity of the module.
	Module uint16
	// Name is the module object name (e.g. alpha.obj).
	Name string
	// Contributions are the module segment contributions.
	Contributions []OMFContribution

	sources []*omfSrcModule
}

// EmbeddedCodeView holds the parsed OMF tables of an NB09/NB11 CodeView
// payload whose debug information is embedded in the PE image. It is nil for
// NB10 external PDB references.
type EmbeddedCodeView struct {
	// DirectoryOffset is the payload-relative offset of the OMF directory.
	DirectoryOffset uint32
	// Entries are the raw OMF directory entries, kept in directory order.
	Entries []OMFDirectoryEntry
	// Segments is the global segment map; index+1 is the logical segment number.
	Segments []OMFSegment
	// Modules are the described modules, sorted by iMod.
	Modules []*EmbeddedModule

	// payload is the full CodeView payload; every reported offset is relative
	// to its first byte.
	payload []byte
	// sections are the PE section headers, used for RVA translation.
	sections []SectionHeader
	// modByID indexes Modules by iMod.
	modByID map[uint16]*EmbeddedModule
	// segByLogical indexes Segments by logical segment number.
	segByLogical map[uint16]OMFSegment
}

// omfSrcModule is a parsed sstSrcModule subsection.
type omfSrcModule struct {
	iMod   uint16
	subOff uint32 // payload offset of the sstSrcModule subsection
	// Parallel per-segment arrays from the sstSrcModule header.
	modStart []uint32
	modEnd   []uint32 // closed interval end
	modSeg   []uint16
	files    []*omfSrcFile
}

// omfSrcFile is one source file record referenced by baseSrcFile.
type omfSrcFile struct {
	recordOff uint32 // payload offset of the source file record (FileOffset)
	name      string
	// Parallel arrays, one entry per source-file segment.
	start  []uint32
	end    []uint32 // closed interval end
	tables []*omfLineTable
}

// omfLineTable is one source line table referenced by baseSrcLn.
type omfLineTable struct {
	seg uint16
	// elemOff[i] is the payload offset of offset[i] in the line table
	// (LineOffset of the resulting source line).
	elemOff []uint32
	offsets []uint32
	lines   []uint16
}

// CodeViewSourceLine is the source-line coverage of one RVA query hit.
type CodeViewSourceLine struct {
	// Module is the iMod of the covering module.
	Module uint16
	// ModuleName is the module object name.
	ModuleName string
	// File is the source file name.
	File string
	// Line is the source line number.
	Line uint16
	// StartRVA is the inclusive start RVA of the half-open coverage interval.
	StartRVA uint32
	// EndRVA is the exclusive end RVA of the half-open coverage interval.
	EndRVA uint32
	// SubsectionOffset is the payload offset of the sstSrcModule subsection.
	SubsectionOffset uint32
	// FileOffset is the payload offset of the source file record.
	FileOffset uint32
	// LineOffset is the payload offset of the offset-array element providing
	// the source line.
	LineOffset uint32
}

// SourceLines reports every source line covering the given RVA. The RVA is
// relative to the image base (ImageBase must not be added). Candidates from
// different modules, files and same-address lines are all retained; an
// uncovered RVA returns nil. Queries do not mutate the debug object.
func (dbg *DebugCodeView) SourceLines(rva uint32) []CodeViewSourceLine {
	em := dbg.Embedded
	if em == nil {
		return nil
	}
	var out []CodeViewSourceLine
	for _, mod := range em.Modules {
		for _, sm := range mod.sources {
			for _, file := range sm.files {
				for k, tbl := range file.tables {
					out = append(out, em.coverModuleTable(rva, mod, sm, file, k, tbl)...)
				}
			}
		}
	}
	sort.SliceStable(out, func(i, j int) bool {
		a, b := out[i], out[j]
		switch {
		case a.Module != b.Module:
			return a.Module < b.Module
		case a.SubsectionOffset != b.SubsectionOffset:
			return a.SubsectionOffset < b.SubsectionOffset
		case a.FileOffset != b.FileOffset:
			return a.FileOffset < b.FileOffset
		case a.LineOffset != b.LineOffset:
			return a.LineOffset < b.LineOffset
		default:
			return a.StartRVA < b.StartRVA
		}
	})
	return out
}

// coverModuleTable evaluates one source line table against the query RVA,
// clipping each line to the module contribution, source module segment,
// source file segment, segment map and PE section ranges.
func (em *EmbeddedCodeView) coverModuleTable(
	rva uint32,
	mod *EmbeddedModule,
	sm *omfSrcModule,
	file *omfSrcFile,
	k int,
	tbl *omfLineTable,
) []CodeViewSourceLine {
	seg, ok := em.segByLogical[tbl.seg]
	if !ok || seg.Frame < 1 || int(seg.Frame) > len(em.sections) {
		return nil
	}
	sect := em.sections[seg.Frame-1]

	// Locate the source module segment range for the table's logical segment.
	mIdx := -1
	for i, s := range sm.modSeg {
		if s == tbl.seg {
			mIdx = i
			break
		}
	}
	if mIdx < 0 {
		return nil
	}

	// Global intersection of all non-contribution ranges, in logical-segment
	// local coordinates. Endpoints are half-open and kept in 64-bit
	// arithmetic so closed-interval ends near 0xFFFFFFFF cannot wrap.
	lo := maxU64(uint64(sm.modStart[mIdx]), uint64(file.start[k]))
	hi := minU64(uint64(sm.modEnd[mIdx])+1, uint64(file.end[k])+1, uint64(seg.Size))

	// The mapped range must stay within the PE section VirtualSize.
	sectEnd := uint64(sect.RelAddr) + uint64(sect.VirtualSize)
	mapBase := uint64(sect.RelAddr) + uint64(seg.Offset)
	if mapBase >= sectEnd {
		return nil
	}
	hi = minU64(hi, sectEnd-mapBase)
	if lo >= hi {
		return nil
	}

	// Each contribution clips independently; gaps between contributions are
	// never filled by neighbouring lines.
	var out []CodeViewSourceLine
	for _, contrib := range mod.Contributions {
		if contrib.Seg != tbl.seg {
			continue
		}
		clo := maxU64(lo, uint64(contrib.Off))
		chi := minU64(hi, uint64(contrib.Off)+uint64(contrib.Size))
		if clo >= chi {
			continue
		}
		for i, lineOff := range tbl.offsets {
			// A line covers [off, next strictly greater off); the last line
			// runs to the closed source-file segment end plus one.
			lineHi := uint64(file.end[k]) + 1
			for j := i + 1; j < len(tbl.offsets); j++ {
				if tbl.offsets[j] > lineOff {
					lineHi = uint64(tbl.offsets[j])
					break
				}
			}
			l := maxU64(uint64(lineOff), clo)
			h := minU64(lineHi, chi)
			if l >= h {
				continue
			}
			startRVA, ok1 := translateRVA(sect.RelAddr, seg.Offset, l)
			endRVA, ok2 := translateRVA(sect.RelAddr, seg.Offset, h)
			if !ok1 || !ok2 {
				continue
			}
			if rva < startRVA || rva >= endRVA {
				continue
			}
			out = append(out, CodeViewSourceLine{
				Module:           mod.Module,
				ModuleName:       mod.Name,
				File:             file.name,
				Line:             tbl.lines[i],
				StartRVA:         startRVA,
				EndRVA:           endRVA,
				SubsectionOffset: sm.subOff,
				FileOffset:       file.recordOff,
				LineOffset:       tbl.elemOff[i],
			})
		}
	}
	return out
}

// translateRVA maps a logical-segment local offset to an RVA, rejecting
// wraparound with wide-integer endpoint arithmetic before narrowing.
func translateRVA(sectRVA, segOffset uint32, local uint64) (uint32, bool) {
	v := uint64(sectRVA) + uint64(segOffset) + local
	if v > math.MaxUint32 {
		return 0, false
	}
	return uint32(v), true
}

func minU64(xs ...uint64) uint64 {
	m := xs[0]
	for _, x := range xs[1:] {
		if x < m {
			m = x
		}
	}
	return m
}

func maxU64(xs ...uint64) uint64 {
	m := xs[0]
	for _, x := range xs[1:] {
		if x > m {
			m = x
		}
	}
	return m
}

// --- [ OMF parsing ] ---------------------------------------------------------

// parseEmbeddedCodeView parses an NB09/NB11 CodeView payload with embedded OMF
// debug information.
func parseEmbeddedCodeView(dbgDir DebugDirectory, payload []byte, sects []SectionHeader) (*DebugCodeView, error) {
	if err := cvCheckPayload(len(payload), 0, 8, "CodeView header"); err != nil {
		return nil, err
	}
	dirOff := binary.LittleEndian.Uint32(payload[4:])
	if err := cvCheckPayload(len(payload), dirOff, 16, "OMF directory header"); err != nil {
		return nil, err
	}
	cbDirHeader, err := cvU16(payload, dirOff, "directory cbDirHeader")
	if err != nil {
		return nil, err
	}
	cbDirEntry, err := cvU16(payload, dirOff+2, "directory cbDirEntry")
	if err != nil {
		return nil, err
	}
	cDir := binary.LittleEndian.Uint32(payload[dirOff+4:])
	lfoNextDir := binary.LittleEndian.Uint32(payload[dirOff+8:])
	if lfoNextDir != 0 {
		return nil, errors.Errorf("codeview: multiple OMF directories not supported (lfoNextDir=0x%x at payload offset 0x%x)", lfoNextDir, dirOff+8)
	}
	if cbDirHeader < 16 {
		return nil, errors.Errorf("codeview: invalid OMF directory header size %d at payload offset 0x%x", cbDirHeader, dirOff)
	}
	if cbDirEntry < 12 {
		return nil, errors.Errorf("codeview: invalid OMF directory entry size %d at payload offset 0x%x", cbDirEntry, dirOff)
	}
	tableEnd := uint64(dirOff) + uint64(cbDirHeader) + uint64(cDir)*uint64(cbDirEntry)
	if tableEnd > uint64(len(payload)) {
		return nil, errors.Errorf("codeview: truncated OMF directory at payload offset 0x%x (ends 0x%x, payload has 0x%x)", dirOff, tableEnd, len(payload))
	}

	em := &EmbeddedCodeView{
		DirectoryOffset: dirOff,
		payload:         payload,
		sections:        sects,
		modByID:         make(map[uint16]*EmbeddedModule),
		segByLogical:    make(map[uint16]OMFSegment),
	}
	for i := uint32(0); i < cDir; i++ {
		eOff := uint64(dirOff) + uint64(cbDirHeader) + uint64(i)*uint64(cbDirEntry)
		entry := OMFDirectoryEntry{
			Subsection: binary.LittleEndian.Uint16(payload[eOff:]),
			Module:     binary.LittleEndian.Uint16(payload[eOff+2:]),
			Offset:     binary.LittleEndian.Uint32(payload[eOff+4:]),
			Size:       binary.LittleEndian.Uint32(payload[eOff+8:]),
		}
		if err := cvCheckPayload(len(payload), entry.Offset, entry.Size, "OMF subsection"); err != nil {
			return nil, err
		}
		em.Entries = append(em.Entries, entry)
	}

	var srcModules []*omfSrcModule
	for _, entry := range em.Entries {
		switch entry.Subsection {
		case cvSstSegMap:
			if err := em.parseSegMap(entry); err != nil {
				return nil, err
			}
		case cvSstModule:
			if err := em.parseModule(entry); err != nil {
				return nil, err
			}
		case cvSstSrcModule:
			sm, err := em.parseSrcModule(entry)
			if err != nil {
				return nil, err
			}
			srcModules = append(srcModules, sm)
		default:
			// Unrecognized subsections are permitted and skipped.
		}
	}
	if err := em.finalize(srcModules); err != nil {
		return nil, err
	}

	return &DebugCodeView{
		DbgDir: dbgDir,
		CodeViewInfo: CodeViewInfo{
			Signature: binary.LittleEndian.Uint32(payload[:4]),
			Offset:    dirOff,
		},
		Embedded: em,
	}, nil
}

// parseSegMap parses an sstSegMap subsection into the global segment map.
func (em *EmbeddedCodeView) parseSegMap(e OMFDirectoryEntry) error {
	if e.Module != cvSegMapGlobalMod {
		return errors.Errorf("codeview: sstSegMap at payload offset 0x%x has iMod %d, want 0x%x", e.Offset, e.Module, cvSegMapGlobalMod)
	}
	base, end := e.Offset, e.Offset+e.Size
	if err := cvCheckSub(base, end, 0, 4, "sstSegMap header"); err != nil {
		return err
	}
	cSeg := binary.LittleEndian.Uint16(em.payload[base:])
	cSegLog := binary.LittleEndian.Uint16(em.payload[base+2:])
	if cSeg != cSegLog {
		return errors.Errorf("codeview: non-flat segment map not supported (cSeg=%d cSegLog=%d at payload offset 0x%x)", cSeg, cSegLog, base)
	}
	if err := cvCheckSub(base, end, 4, uint32(cSeg)*20, "sstSegMap descriptors"); err != nil {
		return err
	}
	for i := uint16(0); i < cSeg; i++ {
		o := base + 4 + uint32(i)*20
		seg := OMFSegment{
			Flags:      binary.LittleEndian.Uint16(em.payload[o:]),
			Overlay:    binary.LittleEndian.Uint16(em.payload[o+2:]),
			Group:      binary.LittleEndian.Uint16(em.payload[o+4:]),
			Frame:      binary.LittleEndian.Uint16(em.payload[o+6:]),
			ISegName:   binary.LittleEndian.Uint16(em.payload[o+8:]),
			IClassName: binary.LittleEndian.Uint16(em.payload[o+10:]),
			Offset:     binary.LittleEndian.Uint32(em.payload[o+12:]),
			Size:       binary.LittleEndian.Uint32(em.payload[o+16:]),
		}
		if seg.Flags != cvSegMapFlatFlags || seg.Overlay != 0 || seg.Group != 0 {
			return errors.Errorf("codeview: segmented sstSegMap descriptor %d not supported (flags=0x%x ovl=%d group=%d at payload offset 0x%x)", i+1, seg.Flags, seg.Overlay, seg.Group, o)
		}
		em.Segments = append(em.Segments, seg)
	}
	return nil
}

// parseModule parses an sstModule subsection into an EmbeddedModule.
func (em *EmbeddedCodeView) parseModule(e OMFDirectoryEntry) error {
	if e.Module == cvSegMapGlobalMod {
		return errors.Errorf("codeview: sstModule at payload offset 0x%x uses reserved iMod 0x%x", e.Offset, cvSegMapGlobalMod)
	}
	base, end := e.Offset, e.Offset+e.Size
	if err := cvCheckSub(base, end, 0, 8, "sstModule header"); err != nil {
		return err
	}
	ovlNumber := binary.LittleEndian.Uint16(em.payload[base:])
	cSeg := binary.LittleEndian.Uint16(em.payload[base+4:])
	style := string(em.payload[base+6 : base+8])
	if ovlNumber != 0 {
		return errors.Errorf("codeview: overlay modules not supported (ovlNumber=%d at payload offset 0x%x)", ovlNumber, base)
	}
	if style != "CV" {
		return errors.Errorf("codeview: unsupported sstModule style %q at payload offset 0x%x", style, base)
	}
	contribEnd := uint32(8) + uint32(cSeg)*12
	if err := cvCheckSub(base, end, 0, contribEnd, "sstModule contributions"); err != nil {
		return err
	}
	mod := &EmbeddedModule{Module: e.Module}
	for i := uint16(0); i < cSeg; i++ {
		o := base + 8 + uint32(i)*12
		mod.Contributions = append(mod.Contributions, OMFContribution{
			Seg:  binary.LittleEndian.Uint16(em.payload[o:]),
			Off:  binary.LittleEndian.Uint32(em.payload[o+4:]),
			Size: binary.LittleEndian.Uint32(em.payload[o+8:]),
		})
	}
	nameOff := base + contribEnd
	if err := cvCheckSub(base, end, contribEnd, 1, "sstModule name length"); err != nil {
		return err
	}
	nameLen := uint32(em.payload[nameOff])
	if err := cvCheckSub(base, end, contribEnd, 1+nameLen, "sstModule name"); err != nil {
		return err
	}
	mod.Name = string(em.payload[nameOff+1 : nameOff+1+nameLen])
	if _, dup := em.modByID[e.Module]; dup {
		return errors.Errorf("codeview: duplicate sstModule for iMod %d at payload offset 0x%x", e.Module, base)
	}
	em.modByID[e.Module] = mod
	em.Modules = append(em.Modules, mod)
	return nil
}

// parseSrcModule parses an sstSrcModule subsection, following baseSrcFile and
// baseSrcLn references rather than any sequential layout.
func (em *EmbeddedCodeView) parseSrcModule(e OMFDirectoryEntry) (*omfSrcModule, error) {
	base, end := e.Offset, e.Offset+e.Size
	if err := cvCheckSub(base, end, 0, 4, "sstSrcModule header"); err != nil {
		return nil, err
	}
	cFile := binary.LittleEndian.Uint16(em.payload[base:])
	cSeg := binary.LittleEndian.Uint16(em.payload[base+2:])

	filesOff := uint32(4)
	pairsOff := filesOff + uint32(cFile)*4
	segOff := pairsOff + uint32(cSeg)*8
	if err := cvCheckSub(base, end, 0, segOff+uint32(cSeg)*2, "sstSrcModule header arrays"); err != nil {
		return nil, err
	}
	sm := &omfSrcModule{iMod: e.Module, subOff: base}
	for i := uint16(0); i < cSeg; i++ {
		o := base + pairsOff + uint32(i)*8
		sm.modStart = append(sm.modStart, binary.LittleEndian.Uint32(em.payload[o:]))
		sm.modEnd = append(sm.modEnd, binary.LittleEndian.Uint32(em.payload[o+4:]))
		sm.modSeg = append(sm.modSeg, binary.LittleEndian.Uint16(em.payload[base+segOff+uint32(i)*2:]))
	}
	for fi := uint16(0); fi < cFile; fi++ {
		rel := binary.LittleEndian.Uint32(em.payload[base+filesOff+uint32(fi)*4:])
		if err := cvCheckSub(base, end, rel, 4, "sstSrcModule baseSrcFile"); err != nil {
			return nil, err
		}
		file, err := em.parseSrcFile(base, end, rel)
		if err != nil {
			return nil, err
		}
		sm.files = append(sm.files, file)
	}
	return sm, nil
}

// parseSrcFile parses one source file record at subBase+rel; all references
// remain relative to the enclosing sstSrcModule subsection start.
func (em *EmbeddedCodeView) parseSrcFile(subBase, subEnd, rel uint32) (*omfSrcFile, error) {
	recOff := subBase + rel
	if err := cvCheckSub(subBase, subEnd, rel, 4, "source file record header"); err != nil {
		return nil, err
	}
	cSeg := binary.LittleEndian.Uint16(em.payload[recOff:])
	lnOff := uint32(4)
	pairsOff := lnOff + uint32(cSeg)*4
	if err := cvCheckSub(subBase, subEnd, rel, pairsOff+uint32(cSeg)*8, "source file record arrays"); err != nil {
		return nil, err
	}
	file := &omfSrcFile{recordOff: recOff}
	for k := uint16(0); k < cSeg; k++ {
		o := recOff + pairsOff + uint32(k)*8
		file.start = append(file.start, binary.LittleEndian.Uint32(em.payload[o:]))
		file.end = append(file.end, binary.LittleEndian.Uint32(em.payload[o+4:]))
	}
	nameRel := rel + pairsOff + uint32(cSeg)*8
	if err := cvCheckSub(subBase, subEnd, nameRel, 1, "source file name length"); err != nil {
		return nil, err
	}
	nameLen := uint32(em.payload[subBase+nameRel])
	if err := cvCheckSub(subBase, subEnd, nameRel, 1+nameLen, "source file name"); err != nil {
		return nil, err
	}
	file.name = string(em.payload[subBase+nameRel+1 : subBase+nameRel+1+nameLen])

	for k := uint16(0); k < cSeg; k++ {
		tableRel := binary.LittleEndian.Uint32(em.payload[recOff+lnOff+uint32(k)*4:])
		if err := cvCheckSub(subBase, subEnd, tableRel, 4, "baseSrcLn line table"); err != nil {
			return nil, err
		}
		tbl, err := em.parseLineTable(subBase, subEnd, tableRel)
		if err != nil {
			return nil, err
		}
		file.tables = append(file.tables, tbl)
	}
	return file, nil
}

// parseLineTable parses one source line table at subBase+tableRel.
func (em *EmbeddedCodeView) parseLineTable(subBase, subEnd, tableRel uint32) (*omfLineTable, error) {
	tblOff := subBase + tableRel
	if err := cvCheckSub(subBase, subEnd, tableRel, 4, "line table header"); err != nil {
		return nil, err
	}
	tbl := &omfLineTable{
		seg: binary.LittleEndian.Uint16(em.payload[tblOff:]),
	}
	cLnOff := binary.LittleEndian.Uint16(em.payload[tblOff+2:])
	offsetsEnd := uint32(4) + uint32(cLnOff)*4
	linesEnd := offsetsEnd + uint32(cLnOff)*2
	if err := cvCheckSub(subBase, subEnd, tableRel, linesEnd, "line table arrays"); err != nil {
		return nil, err
	}
	for i := uint16(0); i < cLnOff; i++ {
		elemOff := tblOff + 4 + uint32(i)*4
		tbl.elemOff = append(tbl.elemOff, elemOff)
		tbl.offsets = append(tbl.offsets, binary.LittleEndian.Uint32(em.payload[elemOff:]))
	}
	for i := uint16(0); i < cLnOff; i++ {
		lineOff := tblOff + offsetsEnd + uint32(i)*2
		tbl.lines = append(tbl.lines, binary.LittleEndian.Uint16(em.payload[lineOff:]))
	}
	return tbl, nil
}

// finalize cross-links source modules with module descriptors and builds the
// logical segment index.
func (em *EmbeddedCodeView) finalize(srcModules []*omfSrcModule) error {
	for i, seg := range em.Segments {
		logical := uint16(i + 1)
		if seg.Frame < 1 || int(seg.Frame) > len(em.sections) {
			return errors.Errorf("codeview: sstSegMap logical segment %d points outside PE sections (frame=%d, sections=%d)", logical, seg.Frame, len(em.sections))
		}
		em.segByLogical[logical] = seg
	}
	for _, sm := range srcModules {
		mod, ok := em.modByID[sm.iMod]
		if !ok {
			return errors.Errorf("codeview: sstSrcModule at payload offset 0x%x references unknown iMod %d", sm.subOff, sm.iMod)
		}
		mod.sources = append(mod.sources, sm)
		for _, seg := range sm.modSeg {
			if _, ok := em.segByLogical[seg]; !ok {
				return errors.Errorf("codeview: sstSrcModule at payload offset 0x%x uses unknown logical segment %d", sm.subOff, seg)
			}
		}
		for _, contrib := range mod.Contributions {
			if _, ok := em.segByLogical[contrib.Seg]; !ok {
				return errors.Errorf("codeview: module %d contribution at unknown logical segment %d", mod.Module, contrib.Seg)
			}
		}
	}
	sort.Slice(em.Modules, func(i, j int) bool { return em.Modules[i].Module < em.Modules[j].Module })
	return nil
}

// --- [ byte bounds helpers ] -------------------------------------------------

func cvCheckPayload(n int, start, size uint32, ctx string) error {
	if uint64(start)+uint64(size) > uint64(n) {
		return errors.Errorf("codeview: truncated %s at payload offset 0x%x (needs %d bytes, payload length %d)", ctx, start, size, n)
	}
	return nil
}

func cvCheckSub(subBase, subEnd, rel, size uint32, ctx string) error {
	start := uint64(subBase) + uint64(rel)
	if start+uint64(size) > uint64(subEnd) {
		return errors.Errorf("codeview: truncated %s at payload offset 0x%x (subsection [0x%x,0x%x), relative offset 0x%x needs %d bytes)", ctx, start, subBase, subEnd, rel, size)
	}
	return nil
}

func cvU16(buf []byte, off uint32, ctx string) (uint16, error) {
	if uint64(off)+2 > uint64(len(buf)) {
		return 0, errors.Errorf("codeview: truncated %s at payload offset 0x%x", ctx, off)
	}
	return binary.LittleEndian.Uint16(buf[off:]), nil
}
