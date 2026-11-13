package line

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"testing"

	"github.com/go-delve/delve/pkg/dwarf/leb128"
)

// ---- helpers to build synthetic line programs ----

type formPair struct {
	contentType uint64
	form        uint64
}

type formVal struct {
	str   string
	u64   uint64
	block []byte
}

func sval(s string) formVal            { return formVal{str: s} }
func uval(v uint64) formVal            { return formVal{u64: v} }
func md5val(b []byte) formVal          { return formVal{block: b} }
func dirPair(ct, form uint64) formPair { return formPair{ct, form} }

func encodeFormVal(w *bytes.Buffer, form uint64, v formVal) {
	switch form {
	case _DW_FORM_string:
		w.WriteString(v.str)
		w.WriteByte(0)
	case _DW_FORM_strp, _DW_FORM_line_strp, _DW_FORM_data4, _DW_FORM_sec_offset:
		binary.Write(w, binary.LittleEndian, uint32(v.u64))
	case _DW_FORM_udata:
		leb128.EncodeUnsigned(w, v.u64)
	case _DW_FORM_data1, _DW_FORM_flag, _DW_FORM_strx1:
		w.WriteByte(byte(v.u64))
	case _DW_FORM_data2, _DW_FORM_strx2:
		binary.Write(w, binary.LittleEndian, uint16(v.u64))
	case _DW_FORM_data8:
		binary.Write(w, binary.LittleEndian, v.u64)
	case _DW_FORM_data16:
		blk := make([]byte, 16)
		copy(blk, v.block)
		w.Write(blk)
	default:
		panic(fmt.Sprintf("encodeFormVal: unsupported form %#x", form))
	}
}

func encodeEntryFormat(format []formPair) []byte {
	var b bytes.Buffer
	b.WriteByte(byte(len(format)))
	for _, f := range format {
		leb128.EncodeUnsigned(&b, f.contentType)
		leb128.EncodeUnsigned(&b, f.form)
	}
	return b.Bytes()
}

func encodeRows(format []formPair, rows [][]formVal) []byte {
	var b bytes.Buffer
	for _, row := range rows {
		if len(row) != len(format) {
			panic("encodeRows: row/format length mismatch")
		}
		for i, f := range format {
			encodeFormVal(&b, f.form, row[i])
		}
	}
	return b.Bytes()
}

func stdOpcodeTable() []byte {
	// min_inst_length, max_ops_per_inst, default_is_stmt, line_base(-5),
	// line_range, opcode_base, standard opcode lengths.
	return []byte{1, 1, 1, 0xfb, 14, 13, 0, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0, 1}
}

// buildLine5 assembles a complete DWARF5 line unit (little endian, DWARF32).
func buildLine5(dirFormat []formPair, dirRows [][]formVal, fileFormat []formPair, fileRows [][]formVal, instr []byte) []byte {
	var header bytes.Buffer
	header.Write(stdOpcodeTable())
	header.Write(encodeEntryFormat(dirFormat))
	leb128.EncodeUnsigned(&header, uint64(len(dirRows)))
	header.Write(encodeRows(dirFormat, dirRows))
	header.Write(encodeEntryFormat(fileFormat))
	leb128.EncodeUnsigned(&header, uint64(len(fileRows)))
	header.Write(encodeRows(fileFormat, fileRows))

	var out bytes.Buffer
	binary.Write(&out, binary.LittleEndian, uint32(8+header.Len()+len(instr)))
	binary.Write(&out, binary.LittleEndian, uint16(5))
	out.Write([]byte{8, 0}) // address_size, segment_selector_size
	binary.Write(&out, binary.LittleEndian, uint32(header.Len()))
	out.Write(header.Bytes())
	out.Write(instr)
	return out.Bytes()
}

type fileRow4 struct {
	name        string
	dir, tm, sz uint64
}

func encodeFileRow4(r fileRow4) []byte {
	var b bytes.Buffer
	b.WriteString(r.name)
	b.WriteByte(0)
	leb128.EncodeUnsigned(&b, r.dir)
	leb128.EncodeUnsigned(&b, r.tm)
	leb128.EncodeUnsigned(&b, r.sz)
	return b.Bytes()
}

// buildLine4 assembles a complete DWARF4 line unit (little endian, DWARF32).
func buildLine4(dirs []string, files []fileRow4, instr []byte) []byte {
	var header bytes.Buffer
	header.Write(stdOpcodeTable())
	for _, d := range dirs {
		header.WriteString(d)
		header.WriteByte(0)
	}
	header.WriteByte(0)
	for _, f := range files {
		header.Write(encodeFileRow4(f))
	}
	header.WriteByte(0)

	var out bytes.Buffer
	binary.Write(&out, binary.LittleEndian, uint32(6+header.Len()+len(instr)))
	binary.Write(&out, binary.LittleEndian, uint16(4))
	binary.Write(&out, binary.LittleEndian, uint32(header.Len()))
	out.Write(header.Bytes())
	out.Write(instr)
	return out.Bytes()
}

func dwSetAddress(addr uint64) []byte {
	var b bytes.Buffer
	b.Write([]byte{0, 9, 2})
	binary.Write(&b, binary.LittleEndian, addr)
	return b.Bytes()
}

func dwDefineFile(name string, dir, tm, sz uint64) []byte {
	entry := encodeFileRow4(fileRow4{name, dir, tm, sz})
	var b bytes.Buffer
	b.WriteByte(0)
	leb128.EncodeUnsigned(&b, uint64(1+len(entry)))
	b.WriteByte(DW_LINE_define_file)
	b.Write(entry)
	return b.Bytes()
}

func dwEndSequence() []byte { return []byte{0, 1, 1} }
func dwCopy() []byte        { return []byte{DW_LNS_copy} }

func dwSetFile(n uint64) []byte {
	var b bytes.Buffer
	b.WriteByte(DW_LNS_set_file)
	leb128.EncodeUnsigned(&b, n)
	return b.Bytes()
}

func cat(parts ...[]byte) []byte {
	var b bytes.Buffer
	for _, p := range parts {
		b.Write(p)
	}
	return b.Bytes()
}

func parseV5(t *testing.T, compdir string, program []byte, lineStr, str []byte) *DebugLineInfo {
	t.Helper()
	info := ParseWithDebugStr(compdir, bytes.NewBuffer(program), lineStr, str, nil, 0, false, 8)
	if info == nil {
		t.Fatal("ParseWithDebugStr returned nil")
	}
	return info
}

func checkFiles(t *testing.T, got []FileEntry, wantPaths []string, wantIdx []uint64) {
	t.Helper()
	if len(got) != len(wantPaths) {
		t.Fatalf("got %d file entries, want %d: %+v", len(got), len(wantPaths), got)
	}
	for i := range got {
		if got[i].Path != wantPaths[i] {
			t.Errorf("file[%d].Path = %q, want %q", i, got[i].Path, wantPaths[i])
		}
		if got[i].Index != wantIdx[i] {
			t.Errorf("file[%d].Index = %d, want %d", i, got[i].Index, wantIdx[i])
		}
	}
}

// ---- TestSourceFiles ----

func TestSourceFiles(t *testing.T) {
	t.Run("forms", testSourceFilesForms)
	t.Run("identity", testSourceFilesIdentity)
	t.Run("dynamic", testSourceFilesDynamic)
	t.Run("compatibility", testSourceFilesCompatibility)
}

func testSourceFilesForms(t *testing.T) {
	t.Run("dwarf5_string", func(t *testing.T) {
		// The directory path uses DW_FORM_string; the file path is preceded by
		// directory_index and followed by size so descriptor order is not the
		// table order.
		dirFormat := []formPair{dirPair(_DW_LNCT_path, _DW_FORM_string)}
		dirRows := [][]formVal{{sval("/src")}}
		fileFormat := []formPair{
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
			dirPair(_DW_LNCT_path, _DW_FORM_string),
			dirPair(_DW_LNCT_timestamp, _DW_FORM_udata),
			dirPair(_DW_LNCT_size, _DW_FORM_udata),
		}
		fileRows := [][]formVal{{uval(0), sval("unit.c"), uval(7), uval(8)}}
		info := parseV5(t, "/build", buildLine5(dirFormat, dirRows, fileFormat, fileRows, nil), nil, nil)
		if got, want := info.IncludeDirs, []string{"/src"}; fmt.Sprintf("%q", got) != fmt.Sprintf("%q", want) {
			t.Fatalf("IncludeDirs = %q, want %q", got, want)
		}
		files := info.SourceFiles()
		checkFiles(t, files, []string{"/src/unit.c"}, []uint64{0})
		if files[0].LastModTime != 7 || files[0].Length != 8 {
			t.Errorf("timestamp/size = %d/%d, want 7/8", files[0].LastModTime, files[0].Length)
		}
		if files[0].DirIdx != 0 {
			t.Errorf("DirIdx = %d, want 0", files[0].DirIdx)
		}
	})

	t.Run("dwarf5_strp", func(t *testing.T) {
		str := newStrSection("/generated", "main.c")
		dirFormat := []formPair{dirPair(_DW_LNCT_path, _DW_FORM_strp)}
		dirRows := [][]formVal{{uval(str.off("/generated"))}}
		fileFormat := []formPair{
			dirPair(_DW_LNCT_path, _DW_FORM_strp),
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
		}
		fileRows := [][]formVal{{uval(str.off("main.c")), uval(0)}}
		info := parseV5(t, "/build", buildLine5(dirFormat, dirRows, fileFormat, fileRows, nil), nil, str.raw)
		if got, want := info.IncludeDirs, []string{"/generated"}; fmt.Sprintf("%q", got) != fmt.Sprintf("%q", want) {
			t.Fatalf("IncludeDirs = %q, want %q", got, want)
		}
		checkFiles(t, info.SourceFiles(), []string{"/generated/main.c"}, []uint64{0})
	})

	t.Run("dwarf5_line_strp", func(t *testing.T) {
		lineStr := newStrSection("/src", "prog.c")
		dirFormat := []formPair{
			dirPair(_DW_LNCT_path, _DW_FORM_line_strp),
			dirPair(_DW_LNCT_MD5, _DW_FORM_data16), // descriptor after path must not drop the directory
		}
		dirRows := [][]formVal{{uval(lineStr.off("/src")), md5val([]byte{1, 2, 3})}}
		fileFormat := []formPair{
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
			dirPair(_DW_LNCT_path, _DW_FORM_line_strp),
			dirPair(_DW_LNCT_size, _DW_FORM_udata),
		}
		fileRows := [][]formVal{{uval(0), uval(lineStr.off("prog.c")), uval(99)}}
		info := parseV5(t, "/build", buildLine5(dirFormat, dirRows, fileFormat, fileRows, nil), lineStr.raw, nil)
		if got, want := info.IncludeDirs, []string{"/src"}; fmt.Sprintf("%q", got) != fmt.Sprintf("%q", want) {
			t.Fatalf("IncludeDirs = %q, want %q", got, want)
		}
		checkFiles(t, info.SourceFiles(), []string{"/src/prog.c"}, []uint64{0})
	})

	t.Run("missing_line_str", func(t *testing.T) {
		// No .debug_line_str: the historical placeholder must be used, even
		// though .debug_str is present and has bytes at offset 0 that could
		// otherwise resolve.
		str := newStrSection("/not-a-fallback")
		dirFormat := []formPair{dirPair(_DW_LNCT_path, _DW_FORM_string)}
		dirRows := [][]formVal{{sval("/src")}}
		fileFormat := []formPair{
			dirPair(_DW_LNCT_path, _DW_FORM_line_strp),
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
		}
		fileRows := [][]formVal{{uval(0), uval(0)}}
		info := parseV5(t, "/build", buildLine5(dirFormat, dirRows, fileFormat, fileRows, nil), nil, str.raw)
		files := info.SourceFiles()
		if len(files) != 1 {
			t.Fatalf("got %d files, want 1", len(files))
		}
		want := "/src/<DW_FORM_line_strp without debug_line_str section>"
		if files[0].Path != want {
			t.Errorf("Path = %q, want %q", files[0].Path, want)
		}
		if bytes.Contains([]byte(files[0].Path), []byte("not-a-fallback")) {
			t.Errorf("used .debug_str as fallback for line_strp: %q", files[0].Path)
		}
	})

	t.Run("missing_str", func(t *testing.T) {
		lineStr := newStrSection("/not-a-fallback")
		dirFormat := []formPair{dirPair(_DW_LNCT_path, _DW_FORM_string)}
		dirRows := [][]formVal{{sval("/src")}}
		fileFormat := []formPair{
			dirPair(_DW_LNCT_path, _DW_FORM_strp),
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
		}
		fileRows := [][]formVal{{uval(0), uval(0)}}
		info := parseV5(t, "/build", buildLine5(dirFormat, dirRows, fileFormat, fileRows, nil), lineStr.raw, nil)
		files := info.SourceFiles()
		if len(files) != 1 {
			t.Fatalf("got %d files, want 1", len(files))
		}
		want := "/src/<DW_FORM_strp without debug_str section>"
		if files[0].Path != want {
			t.Errorf("Path = %q, want %q", files[0].Path, want)
		}
		if bytes.Contains([]byte(files[0].Path), []byte("not-a-fallback")) {
			t.Errorf("used .debug_line_str as fallback for strp: %q", files[0].Path)
		}
	})

	t.Run("descriptor_order", func(t *testing.T) {
		// directory_index first, then size and MD5, then the path. The
		// directory must survive descriptors that follow the directory_index,
		// and the file fields must be read in any descriptor order.
		lineStr := newStrSection("/src", "late.c")
		dirFormat := []formPair{
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
			dirPair(_DW_LNCT_size, _DW_FORM_udata),
			dirPair(_DW_LNCT_MD5, _DW_FORM_data16),
			dirPair(_DW_LNCT_path, _DW_FORM_line_strp),
		}
		md5 := []byte{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6}
		dirRows := [][]formVal{{uval(0), uval(0), md5val(md5), uval(lineStr.off("/src"))}}
		fileFormat := []formPair{
			dirPair(_DW_LNCT_size, _DW_FORM_udata),
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
			dirPair(_DW_LNCT_MD5, _DW_FORM_data16),
			dirPair(_DW_LNCT_path, _DW_FORM_line_strp),
		}
		fileRows := [][]formVal{{uval(42), uval(0), md5val(md5), uval(lineStr.off("late.c"))}}
		info := parseV5(t, "/build", buildLine5(dirFormat, dirRows, fileFormat, fileRows, nil), lineStr.raw, nil)
		files := info.SourceFiles()
		checkFiles(t, files, []string{"/src/late.c"}, []uint64{0})
		if files[0].Length != 42 {
			t.Errorf("Length = %d, want 42", files[0].Length)
		}
		if !files[0].HasMD5 || files[0].MD5 != ([16]byte)(arraysTo16(md5)) {
			t.Errorf("MD5 = %x (HasMD5=%v), want %x", files[0].MD5, files[0].HasMD5, md5)
		}
	})

	t.Run("missing_directory", func(t *testing.T) {
		// A directory_index outside the directory table leaves the path
		// relative instead of dropping the entry or panicking.
		fileFormat := []formPair{
			dirPair(_DW_LNCT_path, _DW_FORM_string),
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
		}
		fileRows := [][]formVal{{sval("orphan.c"), uval(7)}}
		info := parseV5(t, "/build", buildLine5(nil, nil, fileFormat, fileRows, nil), nil, nil)
		checkFiles(t, info.SourceFiles(), []string{"orphan.c"}, []uint64{0})
	})

	t.Run("windows_paths", func(t *testing.T) {
		// Path interpretation must not depend on the host OS: a Windows
		// absolute path is not joined with the directory, and normalizing
		// backslashes is opt-in.
		fileFormat := []formPair{
			dirPair(_DW_LNCT_path, _DW_FORM_string),
			dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
		}
		fileRows := [][]formVal{{sval(`C:\src\win.c`), uval(0)}}
		program := buildLine5(nil, nil, fileFormat, fileRows, nil)
		plain := parseV5(t, "/build", program, nil, nil)
		checkFiles(t, plain.SourceFiles(), []string{`C:\src\win.c`}, []uint64{0})
		norm := ParseWithDebugStr("/build", bytes.NewBuffer(program), nil, nil, nil, 0, true, 8)
		checkFiles(t, norm.SourceFiles(), []string{"C:/src/win.c"}, []uint64{0})
	})

	t.Run("dwarf4", func(t *testing.T) {
		program := buildLine4(nil, []fileRow4{
			{name: "one.c", dir: 0, tm: 3, sz: 4},
			{name: "two.c", dir: 0, tm: 5, sz: 6},
		}, nil)
		info := Parse("/build", bytes.NewBuffer(program), nil, nil, 0, false, 8)
		if info == nil {
			t.Fatal("Parse returned nil")
		}
		files := info.SourceFiles()
		checkFiles(t, files, []string{"/build/one.c", "/build/two.c"}, []uint64{1, 2})
		if files[0].LastModTime != 3 || files[1].Length != 6 {
			t.Errorf("timestamps/lengths wrong: %+v", files)
		}
	})
}

func testSourceFilesIdentity(t *testing.T) {
	lineStr := newStrSection("/src")
	str := newStrSection("dup.c", "other.c")

	dirFormat := []formPair{dirPair(_DW_LNCT_path, _DW_FORM_line_strp)}
	dirRows := [][]formVal{{uval(lineStr.off("/src"))}}
	fileFormat := []formPair{
		dirPair(_DW_LNCT_path, _DW_FORM_strp),
		dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
		dirPair(_DW_LNCT_size, _DW_FORM_udata),
		dirPair(_DW_LNCT_MD5, _DW_FORM_data16),
	}
	zeroMD5 := make([]byte, 16)
	nonzero := []byte{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}
	aa := []byte{0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xa0}
	fileRows := [][]formVal{
		{uval(str.off("dup.c")), uval(0), uval(10), md5val(zeroMD5)},
		{uval(str.off("dup.c")), uval(0), uval(20), md5val(nonzero)},
		{uval(str.off("other.c")), uval(0), uval(30), md5val(aa)},
	}
	info := parseV5(t, "/build", buildLine5(dirFormat, dirRows, fileFormat, fileRows, nil), lineStr.raw, str.raw)
	files := info.SourceFiles()
	if len(files) != 3 {
		t.Fatalf("got %d files, want 3", len(files))
	}
	checkFiles(t, files, []string{"/src/dup.c", "/src/dup.c", "/src/other.c"}, []uint64{0, 1, 2})
	if !files[0].HasMD5 || files[0].MD5 != ([16]byte{}) {
		t.Errorf("entry 0: HasMD5=%v MD5=%x, want explicit all-zero", files[0].HasMD5, files[0].MD5)
	}
	if !files[1].HasMD5 || files[1].MD5 != ([16]byte)(arraysTo16(nonzero)) {
		t.Errorf("entry 1: HasMD5=%v MD5=%x, want distinct digest", files[1].HasMD5, files[1].MD5)
	}
	if files[0].MD5 == files[1].MD5 {
		t.Error("entries with the same path but different digests must both be preserved")
	}
	// Lookup keeps its historical last-path-wins behavior; identity must not
	// rely on it.
	if got := info.Lookup["/src/dup.c"]; got != info.FileNames[1] {
		t.Errorf("Lookup[/src/dup.c] = %p, want last entry %p", got, info.FileNames[1])
	}

	// An entry without a DW_LNCT_MD5 descriptor must not be confused with an
	// explicit all-zero digest.
	noMD5Format := []formPair{
		dirPair(_DW_LNCT_path, _DW_FORM_strp),
		dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
	}
	noMD5Rows := [][]formVal{{uval(str.off("other.c")), uval(0)}}
	noMD5 := parseV5(t, "/build", buildLine5(dirFormat, dirRows, noMD5Format, noMD5Rows, nil), lineStr.raw, str.raw)
	nf := noMD5.SourceFiles()
	if len(nf) != 1 {
		t.Fatalf("got %d files in no-MD5 unit, want 1", len(nf))
	}
	if nf[0].HasMD5 {
		t.Error("entry without DW_LNCT_MD5 has HasMD5=true")
	}
	if nf[0].MD5 != ([16]byte{}) {
		t.Errorf("entry without DW_LNCT_MD5 has non-zero MD5 %x", nf[0].MD5)
	}
}

func arraysTo16(b []byte) [16]byte {
	var r [16]byte
	copy(r[:], b)
	return r
}

func testSourceFilesDynamic(t *testing.T) {
	program := buildLine4([]string{"/proj/gen"}, []fileRow4{
		{name: "main.c", dir: 0, tm: 1, sz: 2},
	}, cat(
		dwSetAddress(0x1000),
		dwCopy(),
		dwDefineFile("extra.c", 1, 11, 12),
		dwSetFile(2),
		dwCopy(),
		dwEndSequence(),
	))
	info := ParseWithDebugStr("/proj", bytes.NewBuffer(program), nil, nil, nil, 0, false, 8)
	if info == nil {
		t.Fatal("ParseWithDebugStr returned nil")
	}
	before := info.SourceFiles()
	checkFiles(t, before, []string{"/proj/main.c", "/proj/gen/extra.c"}, []uint64{1, 2})
	if before[1].LastModTime != 11 || before[1].Length != 12 {
		t.Errorf("defined file fields = %d/%d, want 11/12", before[1].LastModTime, before[1].Length)
	}
	if before[1].DirIdx != 1 {
		t.Errorf("defined file DirIdx = %d, want 1", before[1].DirIdx)
	}
	if len(info.FileNames) != 1 {
		t.Errorf("SourceFiles modified the header file table: %d entries", len(info.FileNames))
	}

	// Querying before asking again must not add duplicate defined files.
	if f, n := info.PCToLine(0, 0x1000); f != "/proj/main.c" || n != 1 {
		t.Errorf("PCToLine = %q:%d, want /proj/main.c:1", f, n)
	}
	// A PC in the dynamically defined file must resolve the same way before
	// and after SourceFiles runs.
	lineBefore := info.LineToPCs("/proj/gen/extra.c", 1)
	if len(lineBefore) != 1 || lineBefore[0].PC != 0x1000 {
		t.Fatalf("LineToPCs(defined file) = %+v, want one PC at 0x1000", lineBefore)
	}
	afterQuery := info.SourceFiles()
	if fmt.Sprintf("%+v", afterQuery) != fmt.Sprintf("%+v", before) {
		t.Fatalf("SourceFiles changed after queries:\n before %+v\n after  %+v", before, afterQuery)
	}
	if lineAfter := info.LineToPCs("/proj/gen/extra.c", 1); fmt.Sprintf("%+v", lineAfter) != fmt.Sprintf("%+v", lineBefore) {
		t.Errorf("LineToPCs changed after SourceFiles: %+v vs %+v", lineBefore, lineAfter)
	}
	// The engine's own defined-file list must not grow on repeat calls.
	if again := info.SourceFiles(); len(again) != 2 {
		t.Errorf("SourceFiles returned %d entries on repeat, want 2", len(again))
	}

	// Repeated calls, and caller mutation, must not corrupt later results.
	again := info.SourceFiles()
	if fmt.Sprintf("%+v", again) != fmt.Sprintf("%+v", before) {
		t.Fatalf("repeated SourceFiles differed: %+v vs %+v", again, before)
	}
	again[0].Path = "mutated"
	if got := info.SourceFiles(); got[0].Path != "/proj/main.c" {
		t.Errorf("caller mutation leaked into SourceFiles cache: %q", got[0].Path)
	}
	if len(info.SourceFiles()) != 2 {
		t.Error("duplicate dynamic files across calls")
	}
}

func testSourceFilesCompatibility(t *testing.T) {
	lineStr := newStrSection("/src")
	dirFormat := []formPair{dirPair(_DW_LNCT_path, _DW_FORM_line_strp)}
	dirRows := [][]formVal{{uval(lineStr.off("/src"))}}
	fileFormat := []formPair{
		dirPair(_DW_LNCT_path, _DW_FORM_string),
		dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
	}
	fileRows := [][]formVal{{sval("a.c"), uval(0)}, {sval("a.c"), uval(0)}}
	program := buildLine5(dirFormat, dirRows, fileFormat, fileRows, cat(
		dwSetAddress(0x2000), dwCopy(), dwEndSequence(),
	))

	// Parse (the old entry point) resolves DW_FORM_line_strp the same way.
	viaParse := Parse("/build", bytes.NewBuffer(program), lineStr.raw, nil, 0, false, 8)
	viaNew := parseV5(t, "/build", program, lineStr.raw, nil)
	if fmt.Sprintf("%+v", viaParse.SourceFiles()) != fmt.Sprintf("%+v", viaNew.SourceFiles()) {
		t.Errorf("Parse and ParseWithDebugStr disagree:\n%+v\n%+v", viaParse.SourceFiles(), viaNew.SourceFiles())
	}

	// Lookup keeps the same-path overwrite convention in both entry points.
	for _, info := range []*DebugLineInfo{viaParse, viaNew} {
		if info.Lookup["/src/a.c"] != info.FileNames[1] {
			t.Errorf("Lookup holds %p, want last entry %p", info.Lookup["/src/a.c"], info.FileNames[1])
		}
	}

	// v5 default file selection and PCToLine are unaffected by SourceFiles.
	if f, n := viaNew.PCToLine(0, 0x2000); f != "/src/a.c" || n != 1 {
		t.Errorf("PCToLine = %q:%d, want /src/a.c:1", f, n)
	}
	filesBefore := fmt.Sprintf("%+v", viaNew.SourceFiles())
	if f, n := viaNew.PCToLine(0, 0x2000); f != "/src/a.c" || n != 1 {
		t.Errorf("PCToLine after SourceFiles = %q:%d", f, n)
	}
	if filesAfter := fmt.Sprintf("%+v", viaNew.SourceFiles()); filesAfter != filesBefore {
		t.Errorf("SourceFiles result changed around PCToLine")
	}
	if got := viaNew.LineToPCs("/src/a.c", 1); len(got) != 1 || got[0].PC != 0x2000 {
		t.Errorf("LineToPCs = %+v, want one PC at 0x2000", got)
	}

	// Old Parse must still report DW_FORM_strp as unsupported: it has no
	// .debug_str to resolve it. ParseWithDebugStr resolves it instead.
	str := newStrSection("strp.c")
	strpFormat := []formPair{
		dirPair(_DW_LNCT_path, _DW_FORM_strp),
		dirPair(_DW_LNCT_directory_index, _DW_FORM_udata),
	}
	strpRows := [][]formVal{{uval(str.off("strp.c")), uval(0)}}
	strpProgram := buildLine5(nil, nil, strpFormat, strpRows, nil)
	var logged []string
	logf := func(format string, args ...any) { logged = append(logged, fmt.Sprintf(format, args...)) }
	old := Parse("/build", bytes.NewBuffer(strpProgram), nil, logf, 0, false, 8)
	if old == nil {
		t.Fatal("Parse returned nil for strp program")
	}
	if old.FileNames[0].Path != "" {
		t.Errorf("legacy Parse resolved DW_FORM_strp to %q, want unsupported/empty", old.FileNames[0].Path)
	}
	if len(logged) == 0 {
		t.Error("legacy Parse did not log the unsupported DW_FORM_strp form")
	}
	resolved := ParseWithDebugStr("/build", bytes.NewBuffer(strpProgram), nil, str.raw, nil, 0, false, 8)
	if resolved == nil {
		t.Fatal("ParseWithDebugStr returned nil for strp program")
	}
	if got := resolved.SourceFiles()[0].Path; got != "strp.c" {
		t.Errorf("ParseWithDebugStr strp path = %q, want strp.c", got)
	}

	// ParseAll still parses multiple units and leaves its signature intact.
	unit := buildLine5(nil, nil, []formPair{dirPair(_DW_LNCT_path, _DW_FORM_string)}, [][]formVal{{sval("x.c")}}, nil)
	lines := ParseAll(append(unit, unit...), nil, nil, 0, false, 8)
	if len(lines) != 2 {
		t.Fatalf("ParseAll returned %d units, want 2", len(lines))
	}
	for i, l := range lines {
		files := l.SourceFiles()
		if len(files) != 1 || files[0].Path != "x.c" || files[0].Index != 0 {
			t.Errorf("unit %d SourceFiles = %+v, want one x.c with index 0", i, files)
		}
	}
}

// strSection tracks a .debug_str or .debug_line_str blob.
type strSection struct {
	raw  []byte
	offs map[string]uint64
}

func newStrSection(strs ...string) *strSection {
	s := &strSection{offs: make(map[string]uint64)}
	for _, str := range strs {
		s.off(str)
	}
	return s
}

func (s *strSection) off(str string) uint64 {
	if off, ok := s.offs[str]; ok {
		return off
	}
	off := uint64(len(s.raw))
	s.raw = append(s.raw, str...)
	s.raw = append(s.raw, 0)
	s.offs[str] = off
	return off
}
