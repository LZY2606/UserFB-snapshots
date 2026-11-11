package pe

import (
	"bytes"
	"encoding/binary"
	"reflect"
	"testing"
)

func testImportFixture(cross, fallback bool) *File {
	d := make([]byte, 256)
	u := binary.LittleEndian.PutUint32
	u(d, 0x1040)
	u(d[12:], 0x1060)
	u(d[16:], 0x1040)
	u(d[64:], 0x1080)
	copy(d[96:], "demo.dll\x00")
	copy(d[130:], "harmless\x00")
	if fallback {
		u(d, 0) // OriginalFirstThunk absent; zero timestamp keeps FirstThunk usable
	}
	s := &Section{SectionHeader: SectionHeader{Name: ".idata", VirtualAddress: 0x1000, VirtualSize: 256, Size: 256, Offset: 0x200}}
	s.Replace(bytes.NewReader(d), int64(len(d)))
	h := &OptionalHeader32{Magic: 0x10b, NumberOfRvaAndSizes: 16}
	h.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT] = DataDirectory{VirtualAddress: 0x1000, Size: 40}
	f := &File{FileHeader: FileHeader{Machine: IMAGE_FILE_MACHINE_I386}, OptionalHeader: h, Sections: []*Section{s}}
	if cross {
		u(d[12:], 0x2000)
		u(d[64:], 0x2020)
		n := make([]byte, 64)
		copy(n, "demo.dll\x00")
		copy(n[34:], "harmless\x00")
		s2 := &Section{SectionHeader: SectionHeader{Name: ".names", VirtualAddress: 0x2000, VirtualSize: 64, Size: 64, Offset: 0x1000}}
		s2.Replace(bytes.NewReader(n), 64)
		f.Sections = append(f.Sections, s2)
	}
	return f
}

func TestBaseline(t *testing.T) {
	got, e := testImportFixture(false, false).ImportedSymbols()
	if e != nil || !reflect.DeepEqual(got, []string{"harmless:demo.dll"}) {
		t.Fatalf("%v %v", got, e)
	}
	// The DLL name and hint/name record live in another section: each
	// reference is now followed through its own RVA.
	got, e = testImportFixture(true, false).ImportedSymbols()
	if e != nil || !reflect.DeepEqual(got, []string{"harmless:demo.dll"}) {
		t.Fatalf("cross section import = %v %v", got, e)
	}
	// OriginalFirstThunk == 0 falls back to the FirstThunk lookup table.
	got, e = testImportFixture(false, true).ImportedSymbols()
	if e != nil || !reflect.DeepEqual(got, []string{"harmless:demo.dll"}) {
		t.Fatalf("fallback import = %v %v", got, e)
	}
	// A descriptor that runs past the mapped bytes is an error, not a panic.
	f := testImportFixture(false, false)
	f.Sections[0].Replace(bytes.NewReader(make([]byte, 19)), 19)
	if _, _, _, err := f.ImportDirectoryTable(); err == nil {
		t.Fatal("expected error for short import descriptor")
	}
}
