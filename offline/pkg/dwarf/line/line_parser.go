package line

import (
	"bytes"
	"encoding/binary"
	"path"
	"strings"

	"github.com/go-delve/delve/pkg/dwarf"
	"github.com/go-delve/delve/pkg/dwarf/leb128"
	"github.com/go-delve/delve/pkg/logflags"
)

// DebugLinePrologue prologue of .debug_line data.
type DebugLinePrologue struct {
	UnitLength     uint32
	Version        uint16
	Length         uint32
	MinInstrLength uint8
	MaxOpPerInstr  uint8
	InitialIsStmt  uint8
	LineBase       int8
	LineRange      uint8
	OpcodeBase     uint8
	StdOpLengths   []uint8
}

// DebugLineInfo info of .debug_line data.
type DebugLineInfo struct {
	Prologue     *DebugLinePrologue
	IncludeDirs  []string
	FileNames    []*FileEntry
	Instructions []byte
	Lookup       map[string]*FileEntry

	Logf func(string, ...any)

	// stateMachineCache[pc] is a state machine stopped at pc
	stateMachineCache map[uint64]*StateMachine

	// lastMachineCache[pc] is a state machine stopped at an address after pc
	lastMachineCache map[uint64]*StateMachine

	// debugLineStr is the contents of the .debug_line_str section.
	debugLineStr []byte

	// debugStr is the contents of the .debug_str section. It is only populated
	// by ParseWithDebugStr; Parse and ParseAll leave it nil.
	debugStr []byte

	// debugStrSupported is true if the parser was created with an entry point
	// that knows about the .debug_str section (ParseWithDebugStr). Parse and
	// ParseAll leave it false to preserve their historical handling of
	// DW_FORM_strp as an unsupported form.
	debugStrSupported bool

	// staticBase is the address at which the executable is loaded, 0 for non-PIEs
	staticBase uint64

	// if normalizeBackslash is true all backslashes (\) will be converted into forward slashes (/)
	normalizeBackslash bool
	ptrSize            int
	endSeqIsValid      bool

	// sourceFiles is the cached result of SourceFiles and sourceFilesLoaded
	// records whether it has been computed.
	sourceFiles       []FileEntry
	sourceFilesLoaded bool
}

// FileEntry file entry in File Name Table.
type FileEntry struct {
	Path        string
	DirIdx      uint64
	LastModTime uint64
	Length      uint64

	// MD5 is the MD5 checksum stored in the DWARF5 file table
	// (DW_LNCT_MD5). It is only meaningful if HasMD5 is true.
	MD5 [16]byte

	// HasMD5 is true if the file entry had an explicit DW_LNCT_MD5
	// descriptor. An all-zero MD5 with HasMD5 set is different from an entry
	// without an MD5 checksum at all.
	HasMD5 bool

	// Index is the index of this file in the line program's file table. For
	// DWARF2-4 the first file has index 1, for DWARF5 the first file has
	// index 0.
	Index uint64
}

type DebugLines []*DebugLineInfo

// ParseAll parses all debug_line segments found in data
func ParseAll(data []byte, debugLineStr []byte, logfn func(string, ...any), staticBase uint64, normalizeBackslash bool, ptrSize int) DebugLines {
	var (
		lines = make(DebugLines, 0)
		buf   = bytes.NewBuffer(data)
	)

	// We have to parse multiple file name tables here.
	for buf.Len() > 0 {
		lines = append(lines, Parse("", buf, debugLineStr, logfn, staticBase, normalizeBackslash, ptrSize))
	}

	return lines
}

// Parse parses a single debug_line segment from buf. Compdir is the
// DW_AT_comp_dir attribute of the associated compile unit.
//
// Parse does not have access to the .debug_str section. DW_FORM_strp
// references in DWARF5 directory and file tables are therefore represented
// with a placeholder; use ParseWithDebugStr when both string sections are
// available.
func Parse(compdir string, buf *bytes.Buffer, debugLineStr []byte, logfn func(string, ...any), staticBase uint64, normalizeBackslash bool, ptrSize int) *DebugLineInfo {
	return parseWithSections(compdir, buf, debugLineStr, nil, false, logfn, staticBase, normalizeBackslash, ptrSize)
}

func parseWithSections(compdir string, buf *bytes.Buffer, debugLineStr, debugStr []byte, debugStrSupported bool, logfn func(string, ...any), staticBase uint64, normalizeBackslash bool, ptrSize int) *DebugLineInfo {
	dbl := new(DebugLineInfo)
	dbl.Logf = logfn
	if logfn == nil {
		dbl.Logf = func(string, ...any) {}
	}
	dbl.staticBase = staticBase
	dbl.ptrSize = ptrSize
	dbl.Lookup = make(map[string]*FileEntry)
	dbl.IncludeDirs = append(dbl.IncludeDirs, compdir)

	dbl.stateMachineCache = make(map[uint64]*StateMachine)
	dbl.lastMachineCache = make(map[uint64]*StateMachine)
	dbl.normalizeBackslash = normalizeBackslash
	dbl.debugLineStr = debugLineStr
	dbl.debugStr = debugStr
	dbl.debugStrSupported = debugStrSupported

	parseDebugLinePrologue(dbl, buf)
	if dbl.Prologue.Version >= 5 {
		if !parseIncludeDirs5(dbl, buf) {
			return nil
		}
		if !parseFileEntries5(dbl, buf) {
			return nil
		}
	} else {
		if !parseIncludeDirs2(dbl, buf) {
			return nil
		}
		if !parseFileEntries2(dbl, buf) {
			return nil
		}
	}

	// Instructions size calculation breakdown:
	//   - dbl.Prologue.UnitLength is the length of the entire unit, not including the 4 bytes to represent that length.
	//   - dbl.Prologue.Length is the length of the prologue not including unit length, version or prologue length itself.
	//   - So you have UnitLength - PrologueLength - (version_length_bytes(2) + prologue_length_bytes(4)).
	verDelta := uint32(6)
	if dbl.Prologue.Version >= 5 {
		verDelta = uint32(8)
	}
	dbl.Instructions = buf.Next(int(dbl.Prologue.UnitLength - dbl.Prologue.Length - verDelta))

	return dbl
}

func parseDebugLinePrologue(dbl *DebugLineInfo, buf *bytes.Buffer) {
	p := new(DebugLinePrologue)

	p.UnitLength = binary.LittleEndian.Uint32(buf.Next(4))
	p.Version = binary.LittleEndian.Uint16(buf.Next(2))
	if p.Version >= 5 {
		dbl.ptrSize = int(buf.Next(1)[0])  // address_size
		dbl.ptrSize += int(buf.Next(1)[0]) // segment_selector_size
	}

	p.Length = binary.LittleEndian.Uint32(buf.Next(4))
	p.MinInstrLength = buf.Next(1)[0]
	if p.Version >= 4 {
		p.MaxOpPerInstr = buf.Next(1)[0]
	} else {
		p.MaxOpPerInstr = 1
	}
	p.InitialIsStmt = buf.Next(1)[0]
	p.LineBase = int8(buf.Next(1)[0])
	p.LineRange = buf.Next(1)[0]
	p.OpcodeBase = buf.Next(1)[0]

	p.StdOpLengths = make([]uint8, p.OpcodeBase-1)
	binary.Read(buf, binary.LittleEndian, &p.StdOpLengths)

	dbl.Prologue = p
}

// parseIncludeDirs2 parses the directory table for DWARF version 2 through 4.
func parseIncludeDirs2(info *DebugLineInfo, buf *bytes.Buffer) bool {
	for {
		str, err := dwarf.ReadString(buf)
		if err != nil {
			if info.Logf != nil {
				info.Logf("error reading string: %v", err)
			}
			return false
		}
		if str == "" {
			break
		}

		info.IncludeDirs = append(info.IncludeDirs, str)
	}
	return true
}

// parseIncludeDirs5 parses the directory table for DWARF version 5.
func parseIncludeDirs5(info *DebugLineInfo, buf *bytes.Buffer) bool {
	dirEntryFormReader := readEntryFormat(buf, info.Logf)
	if dirEntryFormReader == nil {
		return false
	}
	dirCount, _ := leb128.DecodeUnsigned(buf)
	info.IncludeDirs = make([]string, 0, dirCount)
	first_DW_FORM_line_strp_bug := true
	for range dirCount {
		dirEntryFormReader.reset()
		var (
			dir     string
			haveDir bool
		)
		for dirEntryFormReader.next(buf) {
			switch dirEntryFormReader.contentType {
			case _DW_LNCT_path:
				if dirEntryFormReader.formCode == _DW_FORM_line_strp && info.debugLineStr == nil && first_DW_FORM_line_strp_bug {
					first_DW_FORM_line_strp_bug = false
					logflags.Bug.Inc()
				}
				dir, haveDir = info.readStringForm(dirEntryFormReader)
			case _DW_LNCT_directory_index:
			case _DW_LNCT_timestamp:
			case _DW_LNCT_size:
			case _DW_LNCT_MD5:
			}
		}
		if dirEntryFormReader.err != nil {
			if info.Logf != nil {
				info.Logf("error reading directory entries table: %v", dirEntryFormReader.err)
			}
			return false
		}
		// Append one entry per row that had a usable path descriptor. Rows with
		// an unsupported path form are skipped, like Parse has always done;
		// descriptors that follow the path descriptor (directory_index, size,
		// MD5, ...) must not cause the directory to be dropped.
		if haveDir {
			info.IncludeDirs = append(info.IncludeDirs, dir)
		}
	}
	return true
}

// parseFileEntries2 parses the file table for DWARF 2 through 4
func parseFileEntries2(info *DebugLineInfo, buf *bytes.Buffer) bool {
	for {
		entry := readFileEntry(info, buf, true)
		if entry == nil {
			return false
		}
		if entry.Path == "" {
			break
		}

		// DWARF2-4 file indexes are 1-based.
		entry.Index = uint64(len(info.FileNames)) + 1
		info.FileNames = append(info.FileNames, entry)
		info.Lookup[entry.Path] = entry
	}
	return true
}

func readFileEntry(info *DebugLineInfo, buf *bytes.Buffer, exitOnEmptyPath bool) *FileEntry {
	entry := new(FileEntry)

	var err error
	entry.Path, err = dwarf.ReadString(buf)
	if err != nil {
		if info.Logf != nil {
			info.Logf("error reading file entry: %v", err)
		}
		return nil
	}
	if entry.Path == "" && exitOnEmptyPath {
		return entry
	}

	if info.normalizeBackslash {
		entry.Path = strings.ReplaceAll(entry.Path, "\\", "/")
	}

	entry.DirIdx, _ = leb128.DecodeUnsigned(buf)
	entry.LastModTime, _ = leb128.DecodeUnsigned(buf)
	entry.Length, _ = leb128.DecodeUnsigned(buf)
	if !pathIsAbs(entry.Path) {
		if entry.DirIdx < uint64(len(info.IncludeDirs)) {
			entry.Path = path.Join(info.IncludeDirs[entry.DirIdx], entry.Path)
		}
	}

	return entry
}

// pathIsAbs returns true if this is an absolute path.
// We can not use path.IsAbs because it will not recognize windows paths as
// absolute. We also can not use filepath.Abs because we want this
// processing to be independent of the host operating system (we could be
// reading an executable file produced on windows on a unix machine or vice
// versa).
func pathIsAbs(s string) bool {
	if len(s) >= 1 && s[0] == '/' {
		return true
	}
	if len(s) >= 2 && s[1] == ':' && (('a' <= s[0] && s[0] <= 'z') || ('A' <= s[0] && s[0] <= 'Z')) {
		return true
	}
	return false
}

// readStringForm resolves the string value of a DW_LNCT_path descriptor.
//
// DW_FORM_string values are stored inline and have already been read by the
// form reader. DW_FORM_strp values are read from .debug_str and
// DW_FORM_line_strp values from .debug_line_str. If the section a form
// refers to is missing a placeholder is returned instead; the other string
// section is never used as a fallback.
func (info *DebugLineInfo) readStringForm(rdr *formReader) (string, bool) {
	switch rdr.formCode {
	case _DW_FORM_string:
		return rdr.str, true
	case _DW_FORM_line_strp:
		if info.debugLineStr == nil {
			return "<DW_FORM_line_strp without debug_line_str section>", true
		}
		return readSectionString(info.debugLineStr, rdr.u64), true
	case _DW_FORM_strp:
		if !info.debugStrSupported {
			// Parse and ParseAll do not have access to .debug_str; keep their
			// historical behavior for this form.
			info.Logf("unsupported string form %#x", rdr.formCode)
			return "", false
		}
		if info.debugStr == nil {
			return "<DW_FORM_strp without debug_str section>", true
		}
		return readSectionString(info.debugStr, rdr.u64), true
	default:
		info.Logf("unsupported string form %#x", rdr.formCode)
		return "", false
	}
}

// readSectionString reads the null-terminated string at off from section.
func readSectionString(section []byte, off uint64) string {
	if off >= uint64(len(section)) {
		return ""
	}
	rest := section[off:]
	if i := bytes.IndexByte(rest, 0); i >= 0 {
		return string(rest[:i])
	}
	return string(rest)
}

// parseFileEntries5 parses the file table for DWARF 5
func parseFileEntries5(info *DebugLineInfo, buf *bytes.Buffer) bool {
	fileEntryFormReader := readEntryFormat(buf, info.Logf)
	if fileEntryFormReader == nil {
		return false
	}
	fileCount, _ := leb128.DecodeUnsigned(buf)
	info.FileNames = make([]*FileEntry, 0, fileCount)
	for i := 0; i < int(fileCount); i++ {
		var (
			p      string
			diridx = -1

			entry = new(FileEntry)
		)

		fileEntryFormReader.reset()

		for fileEntryFormReader.next(buf) {
			switch fileEntryFormReader.contentType {
			case _DW_LNCT_path:
				p, _ = info.readStringForm(fileEntryFormReader)
			case _DW_LNCT_directory_index:
				diridx = int(fileEntryFormReader.u64)
				entry.DirIdx = fileEntryFormReader.u64
			case _DW_LNCT_timestamp:
				entry.LastModTime = fileEntryFormReader.u64
			case _DW_LNCT_size:
				entry.Length = fileEntryFormReader.u64
			case _DW_LNCT_MD5:
				entry.HasMD5 = true
				entry.MD5 = [16]byte{}
				copy(entry.MD5[:], fileEntryFormReader.block)
			}
		}
		if fileEntryFormReader.err != nil {
			if info.Logf != nil {
				info.Logf("error reading file entries table: %v", fileEntryFormReader.err)
			}
			return false
		}

		if info.normalizeBackslash {
			p = strings.ReplaceAll(p, "\\", "/")
		}
		// Interpret paths using the rules of the target file, not the host: an
		// absolute Unix or Windows path must not be joined with a directory.
		if !pathIsAbs(p) && diridx >= 0 && diridx < len(info.IncludeDirs) {
			p = path.Join(info.IncludeDirs[diridx], p)
		}
		entry.Path = p
		// DWARF5 file indexes are 0-based.
		entry.Index = uint64(i)
		info.FileNames = append(info.FileNames, entry)
		info.Lookup[entry.Path] = entry
	}
	return true
}
