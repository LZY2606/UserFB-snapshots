package line

import "bytes"

// ParseWithDebugStr parses a single DWARF32, little-endian debug_line
// segment from buf. It behaves like Parse but also has access to the
// .debug_str section, so that DW_FORM_strp references in the DWARF5
// directory and file tables can be resolved in addition to DW_FORM_string
// and DW_FORM_line_strp references.
//
// compdir is the DW_AT_comp_dir attribute of the associated compile unit.
// debugLineStr and debugStr are the contents of the .debug_line_str and
// .debug_str sections respectively; a nil slice means that the section is
// missing. staticBase, normalizeBackslash and ptrSize have the same meaning
// as in Parse.
//
// Path references are resolved as follows:
//
//   - DW_FORM_string values are stored inline in the line program.
//   - DW_FORM_line_strp offsets index into debugLineStr.
//   - DW_FORM_strp offsets index into debugStr.
//
// The two string sections are never used as fallbacks for each other. The
// possible outcomes for a DW_LNCT_path descriptor are:
//
//   - A DW_FORM_string form is read inline from the line program itself.
//   - A DW_FORM_line_strp form whose offset is inside debugLineStr is a valid
//     reference and resolves to the string at that offset. Likewise for
//     DW_FORM_strp and debugStr.
//   - A form that refers to a string section that was not supplied (a nil
//     slice) resolves to a placeholder: the historical
//     "<DW_FORM_line_strp without debug_line_str section>" for line_strp, or
//     the distinguishable "<DW_FORM_strp without debug_str section>" for
//     strp.
//   - A form whose offset is outside a section that is present, or whose
//     offsets reach the end of the section without a terminating NUL,
//     resolves to the empty string.
//
// After the line program header is parsed each file table entry has its
// directory resolved the same way as in Parse: absolute paths (including
// Windows paths) are left alone, relative paths with a directory index that
// is outside the directory table are left relative (there is no directory to
// join with), and everything else is joined with the referenced directory.
// Path processing does not depend on the host operating system.
//
// MD5 checksums are preserved verbatim in FileEntry.MD5. An entry with an
// explicit DW_LNCT_MD5 descriptor has HasMD5 set, even if the digest is all
// zero; an entry without that descriptor has HasMD5 unset. The two cases are
// therefore distinguishable.
//
// Split DWARF (DWARF64 and supplementary sections) is not supported.
func ParseWithDebugStr(compdir string, buf *bytes.Buffer, debugLineStr []byte, debugStr []byte, logfn func(string, ...any), staticBase uint64, normalizeBackslash bool, ptrSize int) *DebugLineInfo {
	return parseWithSections(compdir, buf, debugLineStr, debugStr, true, logfn, staticBase, normalizeBackslash, ptrSize)
}

// SourceFiles returns all file entries described by dbl: the entries of the
// line program's file table followed by, for DWARF2 through DWARF4, the
// entries added by DW_LNE_define_file instructions in the line program.
//
// The result is ordered by the file index used by the state machine. In
// DWARF5 the header file table is indexed from 0; in DWARF2-4 it is indexed
// from 1 and DW_LNE_define_file entries continue the numbering after the
// header file table. FileEntry.Index always contains that index.
//
// Entries that have the same path are all preserved: two entries are
// distinct if they have different indexes or different MD5 checksums.
// FileEntry.HasMD5 distinguishes an entry with an explicit all-zero
// DW_LNCT_MD5 from an entry that has no MD5 descriptor at all.
//
// The returned slice belongs to the caller: modifying it does not affect the
// DebugLineInfo, and repeated calls return independent slices. Calling
// SourceFiles does not add duplicate defined files, does not extend
// FileNames or Lookup, and does not change the result of PCToLine, LineToPCs
// or other queries, whether those ran before or after SourceFiles.
//
// DebugLineInfo.Lookup keeps its historical semantics: it maps paths to the
// last file table entry with that path and only contains header file table
// entries. Use the Index field, not Lookup, to identify a file.
func (dbl *DebugLineInfo) SourceFiles() []FileEntry {
	if dbl == nil {
		return nil
	}
	if !dbl.sourceFilesLoaded {
		dbl.sourceFilesLoaded = true
		dbl.sourceFiles = dbl.collectSourceFiles()
	}
	r := make([]FileEntry, len(dbl.sourceFiles))
	copy(r, dbl.sourceFiles)
	return r
}

func (dbl *DebugLineInfo) collectSourceFiles() []FileEntry {
	files := make([]FileEntry, 0, len(dbl.FileNames))
	for _, fe := range dbl.FileNames {
		files = append(files, *fe)
	}
	if dbl.Prologue != nil && dbl.Prologue.Version < 5 {
		// DWARF2-4 files defined with DW_LNE_define_file are not part of the
		// header file table, they only exist in the line program instructions.
		// Run a fresh state machine (the same one used for queries) to find
		// them; this never touches the header tables or the query caches.
		//
		// newStateMachine sets endSeqIsValid, which affects how an
		// end_sequence opcode marks a state machine as valid. Save and restore
		// it so that calling SourceFiles can not change the result of a
		// PCToLine/LineToPCs query run later.
		oldEndSeqIsValid := dbl.endSeqIsValid
		sm := newStateMachine(dbl, dbl.Instructions, dbl.ptrSize)
		for {
			if err := sm.next(); err != nil {
				break
			}
		}
		dbl.endSeqIsValid = oldEndSeqIsValid
		for i, fe := range sm.definedFiles {
			entry := *fe
			entry.Index = uint64(len(dbl.FileNames)) + uint64(i) + 1
			files = append(files, entry)
		}
	}
	return files
}
