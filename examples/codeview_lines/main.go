// Command codeview_lines resolves one or more hexadecimal RVAs to embedded
// CodeView (NB09/NB11) source lines through the production pe package and
// prints TSV records. It never loads or executes the image and never opens an
// external PDB; all records come from the PE's embedded OMF tables.
//
// Usage:
//
//	codeview_lines <pe-path> <rva> [<rva>...]
//
// Each RVA is relative to the image base (do not add ImageBase). Every
// reported subsection/file/line offset is relative to the CodeView payload;
// companion *_pe columns give the same locations translated to PE file
// offsets through the active debug directory.
package main

import (
	"fmt"
	"os"
	"strconv"
	"strings"

	pe "github.com/mewmew/pe"
)

func main() {
	if len(os.Args) < 3 {
		fmt.Fprintf(os.Stderr, "usage: %s <pe-path> <rva> [<rva>...]\n", os.Args[0])
		os.Exit(2)
	}
	pePath := os.Args[1]

	file, err := pe.ParseFile(pePath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "parse %s: %+v\n", pePath, err)
		os.Exit(1)
	}

	var cv *pe.DebugCodeView
	for _, data := range file.DbgData {
		if d, ok := data.(*pe.DebugCodeView); ok {
			cv = d
			break
		}
	}
	if cv == nil {
		fmt.Fprintf(os.Stderr, "%s: no CodeView debug directory\n", pePath)
		os.Exit(1)
	}

	// File offset of the first byte of the CodeView payload in the PE file.
	payloadBase, ok := payloadFileBase(file, cv)
	if !ok {
		fmt.Fprintf(os.Stderr, "%s: unable to locate CodeView payload in file\n", pePath)
		os.Exit(1)
	}

	fmt.Println(strings.Join([]string{
		"sample", "signature", "rva", "module", "module_name", "file", "line",
		"start_rva", "end_rva", "subsection_offset", "file_offset", "line_offset",
		"subsection_pe", "file_pe", "line_pe",
	}, "\t"))

	for _, arg := range os.Args[2:] {
		rva64, err := strconv.ParseUint(arg, 0, 32)
		if err != nil {
			fmt.Fprintf(os.Stderr, "invalid RVA %q: %v\n", arg, err)
			os.Exit(2)
		}
		rva := uint32(rva64)
		lines := cv.SourceLines(rva)
		if len(lines) == 0 {
			// Keep the column count stable; the marker occupies the module
			// column and the remaining fields are empty.
			fmt.Printf("%s\t%s\t%#x\tUNCOVERED\t\t\t\t\t\t\t\t\t\t\t\n",
				pePath, signatureString(cv.Signature), rva)
			continue
		}
		for _, l := range lines {
			fmt.Printf("%s\t%s\t%#x\t%d\t%s\t%s\t%d\t%#x\t%#x\t%#x\t%#x\t%#x\t%#x\t%#x\t%#x\n",
				pePath, signatureString(cv.Signature), rva,
				l.Module, l.ModuleName, l.File, l.Line,
				l.StartRVA, l.EndRVA,
				l.SubsectionOffset, l.FileOffset, l.LineOffset,
				payloadBase+l.SubsectionOffset,
				payloadBase+l.FileOffset,
				payloadBase+l.LineOffset)
		}
	}
}

// payloadFileBase resolves the PE file offset of the CodeView payload from the
// active debug directory. A debug directory may name the payload by file
// pointer or by RVA; an RVA is translated through the section table rather
// than treated as a file coordinate.
func payloadFileBase(file *pe.File, cv *pe.DebugCodeView) (uint32, bool) {
	dir := cv.DbgDir
	if dir.Offset != 0 {
		return dir.Offset, true
	}
	rva := dir.RelAddr
	for _, sect := range file.SectHdrs {
		if rva >= sect.RelAddr && rva < sect.RelAddr+sect.VirtualSize {
			return sect.DataOffset + (rva - sect.RelAddr), true
		}
	}
	return 0, false
}

func signatureString(sig uint32) string {
	return string([]byte{
		byte(sig),
		byte(sig >> 8),
		byte(sig >> 16),
		byte(sig >> 24),
	})
}
