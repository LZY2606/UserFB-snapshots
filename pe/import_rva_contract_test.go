package pe

// RVA resolution contract tests for the normal and delay import parsers.
//
// The test PE images are generated entirely in Go: no Windows, no external
// binaries, no downloads. Each image is produced in two layouts:
//
//   - disk layout: section raw data lives at PointerToRawData, parsed through
//     NewFile;
//   - mapped (memory) layout: the same section content is placed at its RVA,
//     parsed through NewFileFromMemory.
//
// The dependency lists must be identical from both layouts, because every
// reference is followed through its own RVA rather than treated as a common
// file offset.
//
// Mapping boundaries:
//   - only bytes backed by real section raw data are readable; the interval
//     ends at min(VirtualSize, SizeOfRawData), so VirtualSize-only tails and
//     sections with PointerToRawData == 0 never synthesize zero metadata;
//   - gaps between sections, overlapping (ambiguous) intervals and any RVA
//     outside the mapped intervals produce an error naming the RVA;
//   - a single read may cross the boundary of two adjacent, fully backed
//     sections, which is exercised by the split-layout descriptor and the
//     straddling thunk array.
//
// Compatibility:
//   - public API signatures, the "name:dll" symbol format and descriptor and
//     symbol ordering are unchanged;
//   - ordinal thunks are still consumed in order but omitted from the named
//     symbol list;
//   - images without an import directory still return empty results;
//   - corrupted inputs return an error and never a partially populated list,
//     and parsing one bad image leaves no state that affects later files.

import (
	"bytes"
	"encoding/binary"
	"reflect"
	"strings"
	"testing"
)

const (
	testFileAlignment    = 0x200
	testSectionAlignment = 0x1000
	testPEOffset         = 0x80
)

type testSection struct {
	name        string
	rva         uint32
	virtualSize uint32 // 0 means len(raw)
	raw         []byte
	noRaw       bool // uninitialised section: no raw bytes
}

type testImageSpec struct {
	pe64      bool
	imageBase uint64
	sections  []testSection
	importDir DataDirectory
	delayDir  DataDirectory
}

func testAlignUp(v, a uint32) uint32 {
	return (v + a - 1) / a * a
}

func put32(b []byte, v uint32) { binary.LittleEndian.PutUint32(b, v) }
func put64(b []byte, v uint64) { binary.LittleEndian.PutUint64(b, v) }

func putCString(b []byte, at int, s string) {
	copy(b[at:], s)
	b[at+len(s)] = 0
}

// putHintName writes a 2-byte hint followed by a NUL-terminated name.
func putHintName(b []byte, at int, hint uint16, name string) {
	binary.LittleEndian.PutUint16(b[at:], hint)
	putCString(b, at+2, name)
}

// assemble builds a complete PE in both disk and memory-mapped layouts.
func (spec testImageSpec) assemble(t *testing.T) (disk, mem []byte) {
	t.Helper()
	n := len(spec.sections)

	optSize := binary.Size(OptionalHeader32{})
	machine := uint16(IMAGE_FILE_MACHINE_I386)
	if spec.pe64 {
		optSize = binary.Size(OptionalHeader64{})
		machine = IMAGE_FILE_MACHINE_AMD64
	}
	headers := testAlignUp(uint32(testPEOffset+4+binary.Size(FileHeader{})+optSize+40*n), testFileAlignment)

	type rawLoc struct{ off, aligned uint32 }
	locs := make([]rawLoc, n)
	next := headers
	var maxRvaEnd uint64
	for i, s := range spec.sections {
		vs := s.virtualSize
		if vs == 0 {
			vs = uint32(len(s.raw))
		}
		if uint64(s.rva)+uint64(vs) > maxRvaEnd {
			maxRvaEnd = uint64(s.rva) + uint64(vs)
		}
		if s.noRaw {
			continue
		}
		size := testAlignUp(uint32(len(s.raw)), testFileAlignment)
		locs[i] = rawLoc{next, size}
		next += size
	}
	sizeOfImage := testAlignUp(uint32(maxRvaEnd), testSectionAlignment)

	disk = make([]byte, next)
	// DOS header.
	put16 := func(off int, v uint16) { binary.LittleEndian.PutUint16(disk[off:], v) }
	put16(0, 0x5A4D) // MZ
	put32(disk[60:], testPEOffset)
	// PE signature.
	copy(disk[testPEOffset:], []byte{'P', 'E', 0, 0})
	// COFF header.
	fhOff := testPEOffset + 4
	put16(fhOff+0, machine)
	put16(fhOff+2, uint16(n))
	put16(fhOff+16, uint16(optSize))
	put16(fhOff+18, 0x0102) // EXECUTABLE_IMAGE | 32BIT_MACHINE
	// Optional header.
	ohOff := fhOff + binary.Size(FileHeader{})
	if spec.pe64 {
		oh := OptionalHeader64{
			Magic:               0x20b,
			ImageBase:           spec.imageBase,
			SectionAlignment:    testSectionAlignment,
			FileAlignment:       testFileAlignment,
			SizeOfImage:         sizeOfImage,
			SizeOfHeaders:       headers,
			Subsystem:           3,
			NumberOfRvaAndSizes: 16,
		}
		oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = spec.importDir
		oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT] = spec.delayDir
		var buf bytes.Buffer
		if err := binary.Write(&buf, binary.LittleEndian, &oh); err != nil {
			t.Fatalf("write optional header: %v", err)
		}
		copy(disk[ohOff:], buf.Bytes())
	} else {
		oh := OptionalHeader32{
			Magic:               0x10b,
			ImageBase:           uint32(spec.imageBase),
			SectionAlignment:    testSectionAlignment,
			FileAlignment:       testFileAlignment,
			SizeOfImage:         sizeOfImage,
			SizeOfHeaders:       headers,
			Subsystem:           3,
			NumberOfRvaAndSizes: 16,
		}
		oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = spec.importDir
		oh.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT] = spec.delayDir
		var buf bytes.Buffer
		if err := binary.Write(&buf, binary.LittleEndian, &oh); err != nil {
			t.Fatalf("write optional header: %v", err)
		}
		copy(disk[ohOff:], buf.Bytes())
	}
	// Section headers.
	shOff := ohOff + optSize
	for i, s := range spec.sections {
		off := shOff + 40*i
		name := [8]byte{}
		copy(name[:], s.name)
		copy(disk[off:off+8], name[:])
		vs := s.virtualSize
		if vs == 0 {
			vs = uint32(len(s.raw))
		}
		put32(disk[off+8:], vs)
		put32(disk[off+12:], s.rva)
		if !s.noRaw {
			put32(disk[off+16:], locs[i].aligned)
			put32(disk[off+20:], locs[i].off)
		}
		put32(disk[off+36:], 0xC0000040) // READ | INITIALIZED_DATA
	}
	// Disk raw data.
	for i, s := range spec.sections {
		if s.noRaw {
			continue
		}
		copy(disk[locs[i].off:], s.raw)
	}

	// Memory-mapped layout: headers at zero, section bytes at their RVAs.
	mem = make([]byte, sizeOfImage)
	copy(mem, disk[:headers])
	for _, s := range spec.sections {
		if s.noRaw {
			continue
		}
		copy(mem[s.rva:], s.raw)
	}
	return disk, mem
}

func openDiskPE(t *testing.T, b []byte) *File {
	t.Helper()
	f, err := NewFile(bytes.NewReader(b))
	if err != nil {
		t.Fatalf("NewFile: %v", err)
	}
	return f
}

func openMemoryPE(t *testing.T, b []byte) *File {
	t.Helper()
	f, err := NewFileFromMemory(bytes.NewReader(b))
	if err != nil {
		t.Fatalf("NewFileFromMemory: %v", err)
	}
	return f
}

// splitLayoutSpec builds a PE32 image whose descriptors, DLL names and
// thunk arrays live in different sections. The descriptor table straddles
// the .t1/.t2 boundary and the second thunk array straddles .t2/.t3.
func splitLayoutSpec() testImageSpec {
	names := make([]byte, 0x200)
	putCString(names, 0x00, "demo.dll")   // RVA 0x2000
	putCString(names, 0x10, "helper.dll") // RVA 0x2010
	putHintName(names, 0x20, 1, "alpha")  // RVA 0x2020
	putHintName(names, 0x30, 2, "beta")   // RVA 0x2030

	t1 := make([]byte, 0x100) // RVA 0x4000-0x4100
	t2 := make([]byte, 0x100) // RVA 0x4100-0x4200
	t3 := make([]byte, 0x100) // RVA 0x4200-0x4300

	// Import directory at 0x40F0: descriptor A crosses the 0x4100 boundary
	// (16 bytes in .t1, its FirstThunk field in .t2); descriptor B and the
	// zero terminator live in .t2.
	descA := t1[0xF0:]
	put32(descA[0:], 0x4130)  // OriginalFirstThunk (INT A, in .t2)
	put32(descA[4:], 0)       // TimeDateStamp
	put32(descA[8:], 0)       // ForwarderChain
	put32(descA[12:], 0x2000) // NameRVA, in .names
	put32(t2[0:], 0x4040)     // FirstThunk (IAT A, in .t1)
	descB := t2[0x04:]
	put32(descB[0:], 0x41FC) // OriginalFirstThunk (INT B, straddles .t2/.t3)
	put32(descB[12:], 0x2010)
	put32(descB[16:], 0x4050)
	// descriptor at 0x4118 stays zero: terminator.

	// INT A at 0x4130: ordinal first, named symbol after it. The ordinal
	// must be consumed without shifting the following name out of place.
	intA := t2[0x30:]
	put32(intA[0:], 0x80000003)
	put32(intA[4:], 0x2020)
	put32(intA[8:], 0)

	// INT B at 0x41FC: one entry in .t2, zero terminator in .t3.
	intB := t2[0xFC:]
	put32(intB[0:], 0x2030)

	return testImageSpec{
		imageBase: 0x400000,
		sections: []testSection{
			{name: ".names", rva: 0x2000, raw: names},
			{name: ".t1", rva: 0x4000, raw: t1},
			{name: ".t2", rva: 0x4100, raw: t2},
			{name: ".t3", rva: 0x4200, raw: t3},
		},
		importDir: DataDirectory{VirtualAddress: 0x40F0, Size: 60},
	}
}

func TestImportRVAContract_SplitLayout(t *testing.T) {
	wantLibs := []string{"demo.dll", "helper.dll"}
	wantSyms := []string{"alpha:demo.dll", "beta:helper.dll"}

	spec := splitLayoutSpec()
	check := func(t *testing.T, f *File) {
		libs, err := f.ImportedLibraries()
		if err != nil {
			t.Fatalf("ImportedLibraries: %v", err)
		}
		if !reflect.DeepEqual(libs, wantLibs) {
			t.Fatalf("libraries = %v, want %v", libs, wantLibs)
		}
		syms, err := f.ImportedSymbols()
		if err != nil {
			t.Fatalf("ImportedSymbols: %v", err)
		}
		if !reflect.DeepEqual(syms, wantSyms) {
			t.Fatalf("symbols = %v, want %v", syms, wantSyms)
		}
		descs, sec, _, err := f.ImportDirectoryTable()
		if err != nil {
			t.Fatalf("ImportDirectoryTable: %v", err)
		}
		if sec == nil || sec.Name != ".t1" {
			t.Fatalf("directory section = %v, want .t1", sec)
		}
		if len(descs) != 2 || descs[0].DllName != "demo.dll" || descs[1].DllName != "helper.dll" {
			t.Fatalf("descriptors = %+v", descs)
		}
		if descs[0].NameRVA != 0x2000 || descs[0].OriginalFirstThunk != 0x4130 {
			t.Fatalf("raw descriptor fields altered: %+v", descs[0])
		}
	}

	disk, mem := spec.assemble(t)
	t.Run("disk", func(t *testing.T) { check(t, openDiskPE(t, disk)) })
	t.Run("memory", func(t *testing.T) { check(t, openMemoryPE(t, mem)) })

	// Section order permutation must not change the result.
	perm := spec
	perm.sections = []testSection{
		spec.sections[3], spec.sections[1], spec.sections[0], spec.sections[2],
	}
	disk2, mem2 := perm.assemble(t)
	for name, b := range map[string][]byte{"disk": disk2, "memory": mem2} {
		f := openDiskPE(t, b)
		if name == "memory" {
			f = openMemoryPE(t, b)
		}
		libs, err := f.ImportedLibraries()
		if err != nil {
			t.Fatalf("perm/%s libraries: %v", name, err)
		}
		syms, err := f.ImportedSymbols()
		if err != nil {
			t.Fatalf("perm/%s symbols: %v", name, err)
		}
		if !reflect.DeepEqual(libs, wantLibs) || !reflect.DeepEqual(syms, wantSyms) {
			t.Fatalf("perm/%s = %v / %v, want %v / %v", name, libs, syms, wantLibs, wantSyms)
		}
	}
}

// fallbackSpec builds an image whose OriginalFirstThunk is zero.
func fallbackSpec(bound bool) testImageSpec {
	r := make([]byte, 0x200)
	putCString(r, 0x10, "fall.dll") // RVA 0x1010
	putHintName(r, 0x20, 1, "one")  // RVA 0x1020
	putHintName(r, 0x30, 2, "two")  // RVA 0x1030
	put32(r[0x40:], 0x1020)         // FirstThunk array @0x1040
	put32(r[0x44:], 0x1030)
	// 0x1048 stays zero: array terminator.
	desc := r[0x80:]
	put32(desc[0:], 0) // OriginalFirstThunk absent
	if bound {
		put32(desc[4:], 0x12345678) // bound IAT, not a lookup table
	}
	put32(desc[12:], 0x1010)
	put32(desc[16:], 0x1040)
	return testImageSpec{
		imageBase: 0x400000,
		sections:  []testSection{{name: ".rdata", rva: 0x1000, raw: r}},
		importDir: DataDirectory{VirtualAddress: 0x1080, Size: 40},
	}
}

func TestImportRVAContract_Fallback(t *testing.T) {
	spec := fallbackSpec(false)
	disk, mem := spec.assemble(t)
	wantSyms := []string{"one:fall.dll", "two:fall.dll"}
	for name, b := range map[string][]byte{"disk": disk, "memory": mem} {
		f := openDiskPE(t, b)
		if name == "memory" {
			f = openMemoryPE(t, mem)
		}
		syms, err := f.ImportedSymbols()
		if err != nil {
			t.Fatalf("%s ImportedSymbols: %v", name, err)
		}
		if !reflect.DeepEqual(syms, wantSyms) {
			t.Fatalf("%s symbols = %v, want %v", name, syms, wantSyms)
		}
	}

	// A bound FirstThunk must be reported as an error instead of guessing.
	boundDisk, _ := fallbackSpec(true).assemble(t)
	if _, err := openDiskPE(t, boundDisk).ImportedSymbols(); err == nil {
		t.Fatal("bound fallback thunk: expected error, got nil")
	} else if !strings.Contains(err.Error(), "fall.dll") {
		t.Fatalf("bound error %q does not identify the library", err)
	}
	// Library names do not require thunk interpretation and still resolve.
	libs, err := openDiskPE(t, boundDisk).ImportedLibraries()
	if err != nil || !reflect.DeepEqual(libs, []string{"fall.dll"}) {
		t.Fatalf("bound libraries = %v, %v", libs, err)
	}
}

// delaySpec builds a one-descriptor delay import directory.
func delaySpec(pe64 bool, base uint64, grAttrs, nameField uint32) testImageSpec {
	r := make([]byte, 0x200)
	putCString(r, 0x20, "late.dll") // RVA 0x1020
	d := r[0x80:]                   // delay directory at RVA 0x1080
	put32(d[0:], grAttrs)
	put32(d[4:], nameField) // RVADLLName (or VADLLName)
	put32(d[8:], 0x1010)    // RVAHmod
	put32(d[12:], 0x1014)   // RVAIAT
	put32(d[16:], 0x1018)   // RVAINT
	// 0x10A0 descriptor stays zero: terminator.
	return testImageSpec{
		pe64:      pe64,
		imageBase: base,
		sections:  []testSection{{name: ".rdata", rva: 0x1000, raw: r}},
		delayDir:  DataDirectory{VirtualAddress: 0x1080, Size: 64},
	}
}

func TestImportRVAContract_Delay(t *testing.T) {
	const nameRVA uint32 = 0x1020
	cases := []struct {
		name      string
		spec      testImageSpec
		grAttrs   uint32
		nameField uint32
	}{
		{"pe32-rva", delaySpec(false, 0x400000, 1, nameRVA), 1, nameRVA},
		{"pe32-va", delaySpec(false, 0x400000, 0, 0x400000+nameRVA), 0, 0x400000 + nameRVA},
		{"pe32plus-rva", delaySpec(true, 0x140000000, 1, nameRVA), 1, nameRVA},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			disk, mem := tc.spec.assemble(t)
			for layout, b := range map[string][]byte{"disk": disk, "memory": mem} {
				f := openDiskPE(t, b)
				if layout == "memory" {
					f = openMemoryPE(t, b)
				}
				descs, sec, _, err := f.ImportDelayDirectoryTable()
				if err != nil {
					t.Fatalf("%s ImportDelayDirectoryTable: %v", layout, err)
				}
				if sec == nil || sec.Name != ".rdata" || len(descs) != 1 {
					t.Fatalf("%s unexpected table: sec=%v descs=%d", layout, sec, len(descs))
				}
				if descs[0].DllName != "late.dll" {
					t.Fatalf("%s DllName = %q", layout, descs[0].DllName)
				}
				// Raw fields are preserved; only DllName is resolved.
				if descs[0].RVADLLName != tc.nameField || descs[0].GrAttrs != tc.grAttrs {
					t.Fatalf("%s raw descriptor altered: %+v", layout, descs[0])
				}
				libs, err := f.ImportedDelayLibraries()
				if err != nil || !reflect.DeepEqual(libs, []string{"late.dll"}) {
					t.Fatalf("%s delay libraries = %v, %v", layout, libs, err)
				}
			}
		})
	}

	// A VA below the image base cannot be turned into an RVA: truncation or
	// underflow must not fabricate a plausible address.
	badDisk, _ := delaySpec(false, 0x400000, 0, 0x1000).assemble(t)
	f := openDiskPE(t, badDisk)
	if _, err := f.ImportedDelayLibraries(); err == nil {
		t.Fatal("unexpressable delay VA: expected error, got nil")
	}
	descs, _, _, err := f.ImportDelayDirectoryTable()
	if err == nil {
		t.Fatal("unexpressable delay VA: expected directory error, got nil")
	}
	if descs != nil {
		t.Fatalf("delay error returned partial descriptors: %v", descs)
	}
}

// goodBoundsSpec is the well-formed image all corruption cases derive from.
func goodBoundsSpec(pe64 bool) testImageSpec {
	r := make([]byte, 0x200)
	putCString(r, 0x08, "good.dll") // RVA 0x1008
	putHintName(r, 0x20, 1, "sym")  // RVA 0x1020
	put32(r[0x40:], 0x1020)         // INT @0x1040, terminator at 0x1044
	desc := r[0x80:]
	put32(desc[0:], 0x1040) // OriginalFirstThunk
	put32(desc[12:], 0x1008)
	put32(desc[16:], 0x1060)
	machine := uint64(0x400000)
	if pe64 {
		machine = 0x140000000
	}
	return testImageSpec{
		pe64:      pe64,
		imageBase: machine,
		sections:  []testSection{{name: ".rdata", rva: 0x1000, raw: r}},
		importDir: DataDirectory{VirtualAddress: 0x1080, Size: 40},
	}
}

func assertImportError(t *testing.T, f *File, category string) {
	t.Helper()
	if _, err := f.ImportedLibraries(); err == nil {
		t.Fatalf("%s: expected ImportedLibraries error", category)
	} else if !strings.Contains(err.Error(), "0x") {
		t.Fatalf("%s: error %q lacks an RVA", category, err)
	}
	if syms, err := f.ImportedSymbols(); err == nil {
		t.Fatalf("%s: expected ImportedSymbols error, got %v", category, syms)
	}
	if descs, _, _, err := f.ImportDirectoryTable(); err == nil {
		t.Fatalf("%s: expected ImportDirectoryTable error, got %v", category, descs)
	} else if descs != nil {
		t.Fatalf("%s: partial descriptors returned: %v", category, descs)
	}
}

func TestImportRVAContract_Bounds(t *testing.T) {
	goodDisk, goodMem := goodBoundsSpec(false).assemble(t)

	// Sanity: the base image is valid in both layouts.
	for _, f := range []*File{openDiskPE(t, goodDisk), openMemoryPE(t, goodMem)} {
		if libs, err := f.ImportedLibraries(); err != nil || !reflect.DeepEqual(libs, []string{"good.dll"}) {
			t.Fatalf("good image libraries = %v, %v", libs, err)
		}
		if syms, err := f.ImportedSymbols(); err != nil || !reflect.DeepEqual(syms, []string{"sym:good.dll"}) {
			t.Fatalf("good image symbols = %v, %v", syms, err)
		}
	}

	// No import directory: empty results, no error.
	noDir := goodBoundsSpec(false)
	noDir.importDir = DataDirectory{}
	noDirDisk, _ := noDir.assemble(t)
	f := openDiskPE(t, noDirDisk)
	if descs, sec, data, err := f.ImportDirectoryTable(); err != nil || descs != nil || sec != nil || data != nil {
		t.Fatalf("missing import directory = (%v, %v, %v, %v)", descs, sec, data, err)
	}
	if libs, err := f.ImportedLibraries(); err != nil || libs != nil {
		t.Fatalf("missing import directory libraries = %v, %v", libs, err)
	}
	if syms, err := f.ImportedSymbols(); err != nil || syms != nil {
		t.Fatalf("missing import directory symbols = %v, %v", syms, err)
	}

	// 1. Directory Size leaves no room for the zero terminator.
	noTerm := goodBoundsSpec(false)
	noTerm.importDir.Size = 20
	b, _ := noTerm.assemble(t)
	assertImportError(t, openDiskPE(t, b), "no-terminator")

	// 2. Descriptor run ends in an unmapped gap: only 12 of 20 bytes mapped.
	shortDesc := goodBoundsSpec(false)
	tail := make([]byte, 12)
	put32(tail[0:], 0x1040)
	put32(tail[8:], 0x1008) // NameRVA, the remaining fields are unreachable
	shortDesc.sections = []testSection{{name: ".tail", rva: 0x11F4, raw: tail}}
	shortDesc.importDir = DataDirectory{VirtualAddress: 0x11F4, Size: 20}
	b, _ = shortDesc.assemble(t)
	assertImportError(t, openDiskPE(t, b), "short-descriptor")

	// 3. DLL name without a NUL before the mapping ends.
	noNul := goodBoundsSpec(false)
	r := noNul.sections[0].raw
	for i := 0x1F0; i < 0x200; i++ {
		r[i] = 'X'
	}
	put32(r[0x80+12:], 0x11F0)
	b, _ = noNul.assemble(t)
	assertImportError(t, openDiskPE(t, b), "missing-nul")

	// 4. Short hint: only one byte is available for the hint/name record.
	shortHint := goodBoundsSpec(false)
	put32(shortHint.sections[0].raw[0x40:], 0x11FF)
	b, _ = shortHint.assemble(t)
	if _, err := openDiskPE(t, b).ImportedSymbols(); err == nil {
		t.Fatal("short-hint: expected error")
	}

	// 5a. Truncated 32-bit thunk: the entry itself fits, but its zero
	// terminator lands in the gap.
	trunc32 := goodBoundsSpec(false)
	r = trunc32.sections[0].raw
	put32(r[0x1FC:], 0x1020)
	put32(r[0x80:], 0x11FC)
	b, _ = trunc32.assemble(t)
	if _, err := openDiskPE(t, b).ImportedSymbols(); err == nil {
		t.Fatal("truncated-32-thunk: expected error")
	}

	// 5b. Truncated 64-bit thunk: the entry starts at the last 8 bytes of
	// the mapping and its terminator falls into the gap.
	trunc64 := goodBoundsSpec(true)
	r64 := trunc64.sections[0].raw
	put64(r64[0x1F8:], 0x1020)
	put32(r64[0x80:], 0x11F8)
	b64, _ := trunc64.assemble(t)
	if _, err := openDiskPE(t, b64).ImportedSymbols(); err == nil {
		t.Fatal("truncated-64-thunk: expected error")
	}

	// 6. Integer overflow: a fallback thunk RVA whose first 4-byte entry
	// runs past the uint32 address space.
	overflow := goodBoundsSpec(false)
	r = overflow.sections[0].raw
	put32(r[0x80:], 0)          // no OriginalFirstThunk
	put32(r[0x84:], 0)          // unbound, FirstThunk keeps RVAs
	put32(r[0x90:], 0xFFFFFFFC) // FirstThunk
	b, _ = overflow.assemble(t)
	if _, err := openDiskPE(t, b).ImportedSymbols(); err == nil {
		t.Fatal("overflow: expected error")
	}

	// 7. Reference with no mapping.
	unmapped := goodBoundsSpec(false)
	put32(unmapped.sections[0].raw[0x80+12:], 0x5000)
	b, _ = unmapped.assemble(t)
	assertImportError(t, openDiskPE(t, b), "unmapped-name")

	// 8. A VirtualSize-only section (no raw bytes) must not be treated as
	// a readable, zero-filled mapping.
	vsonly := goodBoundsSpec(false)
	vsonly.sections = append(vsonly.sections, testSection{
		name: ".bss", rva: 0x2000, virtualSize: 0x100, noRaw: true,
	})
	put32(vsonly.sections[0].raw[0x80+12:], 0x2000)
	b, _ = vsonly.assemble(t)
	assertImportError(t, openDiskPE(t, b), "virtualsize-only")

	// 9. Overlapping sections make the mapping ambiguous.
	overlap := goodBoundsSpec(false)
	dup := overlap.sections[0]
	dup.name = ".dup"
	overlap.sections = append(overlap.sections, dup)
	b, _ = overlap.assemble(t)
	assertImportError(t, openDiskPE(t, b), "overlap")

	// 10. A delay directory whose last descriptor has fewer than 32 bytes.
	shortDelay := delaySpec(false, 0x400000, 1, 0x1020)
	delayTail := make([]byte, 16)
	put32(delayTail[0:], 1)
	put32(delayTail[4:], 0x1020)
	shortDelay.sections = []testSection{{name: ".rdata", rva: 0x1000, raw: delayTail}}
	shortDelay.delayDir = DataDirectory{VirtualAddress: 0x1000, Size: 32}
	b, _ = shortDelay.assemble(t)
	if descs, _, _, err := openDiskPE(t, b).ImportDelayDirectoryTable(); err == nil {
		t.Fatalf("short-delay-descriptor: expected error, got %v", descs)
	}

	// 11. A failed parse must leave no residue: a good file reads normally
	// immediately after the corrupted ones.
	libs, err := openDiskPE(t, goodDisk).ImportedLibraries()
	if err != nil || !reflect.DeepEqual(libs, []string{"good.dll"}) {
		t.Fatalf("residual state after bad files: %v, %v", libs, err)
	}
	syms, err := openMemoryPE(t, goodMem).ImportedSymbols()
	if err != nil || !reflect.DeepEqual(syms, []string{"sym:good.dll"}) {
		t.Fatalf("residual state after bad files (memory): %v, %v", syms, err)
	}
}
