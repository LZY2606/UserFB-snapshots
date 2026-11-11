package pe

import (
	"encoding/binary"
	"fmt"
	"math"
)

// ImportDirectory entry
type ImportDirectory struct {
	OriginalFirstThunk uint32
	TimeDateStamp      uint32
	ForwarderChain     uint32
	NameRVA            uint32
	FirstThunk         uint32

	DllName string
}

// ImgDelayDescr entry for delayloaded libraries
type ImgDelayDescr struct {
	GrAttrs,
	RVADLLName,
	RVAHmod,
	RVAIAT,
	RVAINT,
	RVABoundIAT,
	RVAUnloadIAT,
	DwTimeStamp uint32

	DllName string
}

// PE32/PE32+ thunk ordinal flags.
const (
	ordinalFlag32 uint32 = 0x80000000
	ordinalFlag64 uint64 = 1 << 63
)

// GrAttrs flag: when set the delay import descriptor fields hold VAs instead
// of RVAs.
const delayAttrRVA uint32 = 0x00000001

// rvaReader resolves RVAs against the section map of a PE image. Every lookup
// uses the RVA of the referenced datum itself: contiguous RVAs may map to
// discontiguous file offsets, and a single read is allowed to straddle two
// adjacent sections as long as both halves are backed by mapped raw bytes.
// No data is ever invented for gaps, ambiguous overlaps or VirtualSize-only
// regions.
type rvaReader struct {
	f *File
}

type rvaMapping struct {
	data     []byte
	rvaStart uint32
	rvaEnd   uint32 // exclusive, backed by raw bytes
}

func (f *File) newRVAReader() *rvaReader {
	return &rvaReader{f: f}
}

// mappings returns, for each section that carries raw bytes, the RVA interval
// actually backed by those bytes. The interval ends at the smaller of
// VirtualSize and Size so that VirtualSize-only tails (e.g. .bss) never look
// readable.
func (r *rvaReader) mappings() ([]rvaMapping, error) {
	out := make([]rvaMapping, 0, len(r.f.Sections))
	for _, s := range r.f.Sections {
		// A zero PointerToRawData marks an uninitialised section; any bytes
		// the reader might hand back are synthesised zeros rather than raw
		// data, so they must never back a metadata lookup.
		if s.Offset == 0 {
			continue
		}
		data, err := s.Data()
		if err != nil {
			return nil, err
		}
		n := uint64(s.VirtualSize)
		if n == 0 || uint64(len(data)) < n {
			n = uint64(len(data))
		}
		if n == 0 {
			continue
		}
		// VirtualAddress + n must not overflow a uint32.
		if uint64(s.VirtualAddress)+n > math.MaxUint32 {
			return nil, fmt.Errorf("pe: section %q RVA interval overflows at rva %#x", s.Name, s.VirtualAddress)
		}
		out = append(out, rvaMapping{
			data:     data[:n],
			rvaStart: s.VirtualAddress,
			rvaEnd:   s.VirtualAddress + uint32(n),
		})
	}
	return out, nil
}

// mappingFor returns the unique mapping backing rva. Overlapping intervals
// make the address ambiguous and are therefore rejected.
func (r *rvaReader) mappingFor(mappings []rvaMapping, rva uint32) (rvaMapping, error) {
	var found rvaMapping
	matches := 0
	for _, m := range mappings {
		if rva >= m.rvaStart && rva < m.rvaEnd {
			found = m
			matches++
		}
	}
	switch matches {
	case 0:
		return rvaMapping{}, fmt.Errorf("pe: rva %#x is not mapped to any section with raw data", rva)
	case 1:
		return found, nil
	default:
		return rvaMapping{}, fmt.Errorf("pe: rva %#x is ambiguously mapped by overlapping sections", rva)
	}
}

// readAtRVA copies n bytes starting at rva, following the per-RVA section
// map instead of treating an RVA as a file offset. A read may cross the
// boundary between two adjacent, fully backed sections.
func (r *rvaReader) readAtRVA(mappings []rvaMapping, rva uint32, n uint64) ([]byte, error) {
	if n == 0 {
		return nil, nil
	}
	if uint64(rva)+n > math.MaxUint32 {
		return nil, fmt.Errorf("pe: integer overflow reading %d bytes at rva %#x", n, rva)
	}
	out := make([]byte, n)
	cur := rva
	for off := uint64(0); off < n; {
		m, err := r.mappingFor(mappings, cur)
		if err != nil {
			return nil, err
		}
		avail := uint64(m.rvaEnd - cur)
		chunk := n - off
		if chunk > avail {
			chunk = avail
		}
		start := cur - m.rvaStart
		copy(out[off:off+chunk], m.data[start:start+uint32(chunk)])
		off += chunk
		cur += uint32(chunk)
	}
	return out, nil
}

// cstringAtRVA reads a NUL-terminated string at rva.
func (r *rvaReader) cstringAtRVA(mappings []rvaMapping, rva uint32) (string, error) {
	// Bound the search: a string must lie fully inside one of the mapped
	// intervals; scan from rva up to the end of the first mapping that
	// covers it, and continue across a section boundary if needed.
	var buf []byte
	cur := rva
	for {
		m, err := r.mappingFor(mappings, cur)
		if err != nil {
			return "", err
		}
		part := m.data[cur-m.rvaStart:]
		for i, b := range part {
			if b == 0 {
				buf = append(buf, part[:i]...)
				return string(buf), nil
			}
		}
		buf = append(buf, part...)
		cur = m.rvaEnd
	}
}

// isPE32 reports whether the image uses the 4-byte thunk layout.
func (f *File) isPE32() (bool, error) {
	switch f.OptionalHeader.(type) {
	case *OptionalHeader32:
		return true, nil
	case *OptionalHeader64:
		return false, nil
	default:
		return false, fmt.Errorf("pe: unknown optional header type %T", f.OptionalHeader)
	}
}

func (f *File) imageBase() (uint64, error) {
	switch oh := f.OptionalHeader.(type) {
	case *OptionalHeader32:
		return uint64(oh.ImageBase), nil
	case *OptionalHeader64:
		return oh.ImageBase, nil
	default:
		return 0, fmt.Errorf("pe: unknown optional header type %T", f.OptionalHeader)
	}
}

// IAT returns the DataDirectory for the IAT
func (f *File) IAT() *DataDirectory {
	_, idd := f.sectionFromDirectoryEntry(IMAGE_DIRECTORY_ENTRY_IAT)
	return &idd
}

// ImportDirectoryTable - returns the Import Directory Table, a pointer to the section,
// and the section raw data. The returned section and data describe the section holding
// the first import descriptor; referenced DLL names, thunk arrays and hint/name records
// are resolved independently through their own RVAs and may live in other sections.
func (f *File) ImportDirectoryTable() ([]ImportDirectory, *Section, *[]byte, error) {
	ds, idd := f.sectionFromDirectoryEntry(IMAGE_DIRECTORY_ENTRY_IMPORT)

	// didn't find a section, so no import libraries were found
	if ds == nil {
		return nil, nil, nil, nil
	}

	sectionData, err := ds.Data()
	if err != nil {
		return nil, nil, nil, err
	}

	mappings, err := f.newRVAReader().mappings()
	if err != nil {
		return nil, nil, nil, err
	}

	descs, err := f.parseImportDescriptors(mappings, idd)
	if err != nil {
		return nil, nil, nil, err
	}
	return descs, ds, &sectionData, nil
}

// parseImportDescriptors walks the descriptor sequence bounded by the data
// directory Size. A complete zero-filled 20-byte descriptor terminates the
// sequence; the terminator must be reached inside the directory range.
func (f *File) parseImportDescriptors(mappings []rvaMapping, idd DataDirectory) ([]ImportDirectory, error) {
	r := f.newRVAReader()
	if idd.Size > 0 && idd.Size%20 != 0 {
		return nil, fmt.Errorf("pe: import directory size %d at rva %#x is not a multiple of 20", idd.Size, idd.VirtualAddress)
	}
	var ida []ImportDirectory
	for off := uint64(0); idd.Size == 0 || off < uint64(idd.Size); off += 20 {
		entryRva, overflow := rvaOffset(idd.VirtualAddress, off)
		if overflow {
			return nil, fmt.Errorf("pe: integer overflow indexing import directory at rva %#x", idd.VirtualAddress)
		}
		raw, err := r.readAtRVA(mappings, entryRva, 20)
		if err != nil {
			return nil, fmt.Errorf("pe: import directory entry %d: %v", off/20, err)
		}
		dt := ImportDirectory{
			OriginalFirstThunk: binary.LittleEndian.Uint32(raw[0:4]),
			TimeDateStamp:      binary.LittleEndian.Uint32(raw[4:8]),
			ForwarderChain:     binary.LittleEndian.Uint32(raw[8:12]),
			NameRVA:            binary.LittleEndian.Uint32(raw[12:16]),
			FirstThunk:         binary.LittleEndian.Uint32(raw[16:20]),
		}
		// Only a descriptor with all five fields zero terminates the table.
		if dt.OriginalFirstThunk == 0 && dt.TimeDateStamp == 0 && dt.ForwarderChain == 0 &&
			dt.NameRVA == 0 && dt.FirstThunk == 0 {
			return ida, nil
		}
		if dt.NameRVA == 0 {
			return nil, fmt.Errorf("pe: import descriptor %d has no DLL name rva", off/20)
		}
		dt.DllName, err = r.cstringAtRVA(mappings, dt.NameRVA)
		if err != nil {
			return nil, fmt.Errorf("pe: import descriptor %d DLL name at rva %#x: %v", off/20, dt.NameRVA, err)
		}
		ida = append(ida, dt)
	}
	return nil, fmt.Errorf("pe: import directory of size %d at rva %#x has no zero terminator", idd.Size, idd.VirtualAddress)
}

// importThunk selects the lookup table to walk for an import descriptor.
// OriginalFirstThunk (the INT) is authoritative; when it is zero the loader
// falls back to FirstThunk (the IAT), which is only usable while it still
// holds the original RVAs rather than bound addresses.
func importThunk(dt ImportDirectory) (uint32, error) {
	if dt.OriginalFirstThunk != 0 {
		return dt.OriginalFirstThunk, nil
	}
	if dt.FirstThunk == 0 {
		return 0, fmt.Errorf("pe: import of %q has neither an original first thunk nor a first thunk", dt.DllName)
	}
	// A zero TimeDateStamp marks an unbound image whose FirstThunk still
	// carries the original RVAs. Binding replaces the IAT entries with
	// absolute addresses and stamps the field with a non-zero timestamp,
	// which cannot be interpreted as hint/name RVAs.
	if dt.TimeDateStamp != 0 {
		return 0, fmt.Errorf("pe: import of %q is bound and its first thunk is not a lookup table", dt.DllName)
	}
	return dt.FirstThunk, nil
}

// readImportNames walks a thunk array at rva and returns the imported symbol
// names in array order. Ordinal entries are consumed but omitted from the
// list, matching the historical behaviour. Each thunk and hint/name record
// is resolved through its own RVA.
func (f *File) readImportNames(mappings []rvaMapping, rva uint32, pe32 bool) ([]string, error) {
	r := f.newRVAReader()
	width := uint64(8)
	ordinal := ordinalFlag64
	if pe32 {
		width = 4
		ordinal = uint64(ordinalFlag32)
	}
	var names []string
	for i := uint64(0); ; i++ {
		entryRva, overflow := rvaOffset(rva, i*width)
		if overflow {
			return nil, fmt.Errorf("pe: integer overflow indexing import thunk at rva %#x", rva)
		}
		raw, err := r.readAtRVA(mappings, entryRva, width)
		if err != nil {
			return nil, fmt.Errorf("pe: import thunk %d at rva %#x: %v", i, entryRva, err)
		}
		var value uint64
		if pe32 {
			value = uint64(binary.LittleEndian.Uint32(raw))
		} else {
			value = binary.LittleEndian.Uint64(raw)
		}
		if value == 0 {
			return names, nil
		}
		if value&ordinal != 0 {
			// Ordinal import: consume and skip, do not add to named symbols.
			continue
		}
		if value > math.MaxUint32 {
			return nil, fmt.Errorf("pe: import thunk %d value %#x does not fit an RVA", i, value)
		}
		hnRva := uint32(value)
		// A hint/name record starts with a 2-byte hint followed by the
		// NUL-terminated name.
		if _, err := r.readAtRVA(mappings, hnRva, 2); err != nil {
			return nil, fmt.Errorf("pe: import thunk %d hint/name record at rva %#x: %v", i, hnRva, err)
		}
		name, err := r.cstringAtRVA(mappings, hnRva+2)
		if err != nil {
			return nil, fmt.Errorf("pe: import thunk %d name at rva %#x: %v", i, hnRva+2, err)
		}
		names = append(names, name)
	}
}

// ImportedSymbols returns the names of all symbols
// referred to by the binary f that are expected to be
// satisfied by other libraries at dynamic load time.
// It does not return weak symbols.
func (f *File) ImportedSymbols() ([]string, error) {
	pe32, err := f.isPE32()
	if err != nil {
		return nil, err
	}

	ida, _, _, err := f.ImportDirectoryTable()
	if err != nil {
		return nil, err
	}

	mappings, err := f.newRVAReader().mappings()
	if err != nil {
		return nil, err
	}

	var all []string
	for _, dt := range ida {
		thunkRva, err := importThunk(dt)
		if err != nil {
			return nil, err
		}
		names, err := f.readImportNames(mappings, thunkRva, pe32)
		if err != nil {
			return nil, err
		}
		for _, name := range names {
			all = append(all, name+":"+dt.DllName)
		}
	}
	return all, nil
}

// ImportedLibraries returns the names of all libraries
// referred to by the binary f that are expected to be
// linked with the binary at dynamic link time.
func (f *File) ImportedLibraries() ([]string, error) {
	ida, _, _, err := f.ImportDirectoryTable()
	if err != nil {
		return nil, err
	}
	var all []string
	for _, dt := range ida {
		all = append(all, dt.DllName)
	}
	return all, nil
}

func (f File) sectionFromDirectoryEntry(directory uint32) (*Section, DataDirectory) {
	pe64 := f.Machine == IMAGE_FILE_MACHINE_AMD64

	// grab the number of data directory entries
	var ddLength uint32
	if pe64 {
		ddLength = f.OptionalHeader.(*OptionalHeader64).NumberOfRvaAndSizes
	} else {
		ddLength = f.OptionalHeader.(*OptionalHeader32).NumberOfRvaAndSizes
	}

	// check that the length of data directory entries is large
	// enough to include the directory.
	if ddLength < directory+1 {
		return nil, DataDirectory{}
	}

	// grab the directory entry
	var idd DataDirectory
	if pe64 {
		idd = f.OptionalHeader.(*OptionalHeader64).DataDirectory[directory]
	} else {
		idd = f.OptionalHeader.(*OptionalHeader32).DataDirectory[directory]
	}
	if idd.VirtualAddress == 0 {
		return nil, idd
	}

	// figure out which section contains the directory table
	var ds *Section
	for _, s := range f.Sections {
		if s.VirtualAddress <= idd.VirtualAddress && idd.VirtualAddress < s.VirtualAddress+s.VirtualSize {
			ds = s
			break
		}
	}
	return ds, idd
}

// ImportDelayDirectoryTable - returns the delay Import Directory Table, a pointer to the
// section holding the first descriptor, and that section's raw data. Referenced DLL names
// are resolved independently through their own RVAs (or VAs, depending on GrAttrs).
func (f *File) ImportDelayDirectoryTable() ([]ImgDelayDescr, *Section, *[]byte, error) {
	ds, idd := f.sectionFromDirectoryEntry(IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)

	// didn't find a section, so no delay imports were found
	if ds == nil {
		return nil, nil, nil, nil
	}

	sectionData, err := ds.Data()
	if err != nil {
		return nil, nil, nil, err
	}

	mappings, err := f.newRVAReader().mappings()
	if err != nil {
		return nil, nil, nil, err
	}

	descs, err := f.parseDelayDescriptors(mappings, idd)
	if err != nil {
		return nil, nil, nil, err
	}
	return descs, ds, &sectionData, nil
}

// parseDelayDescriptors walks the 32-byte delay descriptor sequence bounded
// by the directory Size. The raw descriptor fields are preserved in the
// returned structures; only DllName is resolved.
func (f *File) parseDelayDescriptors(mappings []rvaMapping, idd DataDirectory) ([]ImgDelayDescr, error) {
	r := f.newRVAReader()
	if idd.Size > 0 && idd.Size%32 != 0 {
		return nil, fmt.Errorf("pe: delay import directory size %d at rva %#x is not a multiple of 32", idd.Size, idd.VirtualAddress)
	}
	base, err := f.imageBase()
	if err != nil {
		return nil, err
	}
	var dida []ImgDelayDescr
	for off := uint64(0); idd.Size == 0 || off < uint64(idd.Size); off += 32 {
		entryRva, overflow := rvaOffset(idd.VirtualAddress, off)
		if overflow {
			return nil, fmt.Errorf("pe: integer overflow indexing delay import directory at rva %#x", idd.VirtualAddress)
		}
		raw, err := r.readAtRVA(mappings, entryRva, 32)
		if err != nil {
			return nil, fmt.Errorf("pe: delay import directory entry %d: %v", off/32, err)
		}
		dt := ImgDelayDescr{
			GrAttrs:      binary.LittleEndian.Uint32(raw[0:4]),
			RVADLLName:   binary.LittleEndian.Uint32(raw[4:8]),
			RVAHmod:      binary.LittleEndian.Uint32(raw[8:12]),
			RVAIAT:       binary.LittleEndian.Uint32(raw[12:16]),
			RVAINT:       binary.LittleEndian.Uint32(raw[16:20]),
			RVABoundIAT:  binary.LittleEndian.Uint32(raw[20:24]),
			RVAUnloadIAT: binary.LittleEndian.Uint32(raw[24:28]),
			DwTimeStamp:  binary.LittleEndian.Uint32(raw[28:32]),
		}
		// The sequence ends on an all-zero descriptor.
		if dt.GrAttrs == 0 && dt.RVADLLName == 0 && dt.RVAHmod == 0 && dt.RVAIAT == 0 &&
			dt.RVAINT == 0 && dt.RVABoundIAT == 0 && dt.RVAUnloadIAT == 0 && dt.DwTimeStamp == 0 {
			return dida, nil
		}
		if dt.RVADLLName == 0 {
			return nil, fmt.Errorf("pe: delay import descriptor %d has no DLL name reference", off/32)
		}
		nameRva, err := f.delayNameRVA(dt, base)
		if err != nil {
			return nil, err
		}
		if dt.DllName, err = r.cstringAtRVA(mappings, nameRva); err != nil {
			return nil, fmt.Errorf("pe: delay import descriptor %d DLL name at rva %#x: %v", off/32, nameRva, err)
		}
		dida = append(dida, dt)
	}
	return nil, fmt.Errorf("pe: delay import directory of size %d at rva %#x has no zero terminator", idd.Size, idd.VirtualAddress)
}

// delayNameRVA converts the DLL name reference of a delay descriptor to an
// RVA. The low bit of GrAttrs selects the encoding: set means RVA, clear
// means absolute VA that must be checked against the image base. VA inputs
// are validated in 64-bit arithmetic so a uint32 truncation or underflow can
// never produce a plausible-looking RVA.
func (f *File) delayNameRVA(dt ImgDelayDescr, base uint64) (uint32, error) {
	if dt.GrAttrs&delayAttrRVA != 0 {
		return dt.RVADLLName, nil
	}
	va := uint64(dt.RVADLLName)
	if va < base {
		return 0, fmt.Errorf("pe: delay import VA %#x is below image base %#x", va, base)
	}
	rva64 := va - base
	if rva64 > math.MaxUint32 {
		return 0, fmt.Errorf("pe: delay import VA %#x minus image base %#x does not fit an RVA", va, base)
	}
	return uint32(rva64), nil
}

// ImportedDelayLibraries returns the names of all libraries referred to by the binary f
// that are added to the delay imports directory. These libraries are not loaded at initialisation,
// but may be loaded during runtime.
func (f *File) ImportedDelayLibraries() ([]string, error) {
	ida, _, _, err := f.ImportDelayDirectoryTable()
	if err != nil {
		return nil, err
	}
	var all []string
	for _, dt := range ida {
		all = append(all, dt.DllName)
	}
	return all, nil
}

// rvaOffset adds a byte offset to an RVA using 64-bit arithmetic and reports
// an overflow instead of silently wrapping.
func rvaOffset(rva uint32, offset uint64) (uint32, bool) {
	s := uint64(rva) + offset
	return uint32(s), s > math.MaxUint32
}
