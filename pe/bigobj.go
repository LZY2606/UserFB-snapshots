package pe

// BigObj (extended COFF) object support. Only the AMD64 BigObj v2 disk
// layout is recognized; the objects are read, never loaded or executed.

// bigObjHeaderSize is the size of the BigObj v2 header on disk.
const bigObjHeaderSize = 56

// bigObjClassID is the ClassID GUID of a BigObj v2 object.
var bigObjClassID = [16]byte{0xc7, 0xa1, 0xba, 0xd1, 0xee, 0xba, 0xa9, 0x4b, 0xaf, 0x20, 0xfa, 0xf6, 0x6a, 0xa4, 0xdc, 0xb8}

// BigObjHeader is the 56-byte header of a BigObj v2 (extended COFF) object.
// A short import object shares the initial 00 00 ff ff signature but has a
// different header and is not a BigObj.
type BigObjHeader struct {
	Sig1                 uint16 // always 0
	Sig2                 uint16 // always 0xffff
	Version              uint16 // only version 2 is supported
	Machine              uint16
	TimeDateStamp        uint32
	ClassID              [16]byte
	SizeOfData           uint32
	Flags                uint32
	MetaDataSize         uint32
	MetaDataOffset       uint32
	NumberOfSections     uint32
	PointerToSymbolTable uint32
	NumberOfSymbols      uint32 // including auxiliary slots
}

// IsBigObj reports whether the file is a BigObj (extended COFF) object
// rather than an ordinary COFF object or PE image.
func (f *File) IsBigObj() bool {
	return f.BigObjHeader != nil
}

// SectionCount returns the real number of sections in the file. For
// ordinary files this is FileHeader.NumberOfSections; for BigObj files it
// is the full 32-bit section count, which may exceed 65535.
func (f *File) SectionCount() uint32 {
	if f.BigObjHeader != nil {
		return f.BigObjHeader.NumberOfSections
	}
	return uint32(f.FileHeader.NumberOfSections)
}

// coffSymbolStride returns the on-disk size of one COFF symbol record:
// 20 bytes for BigObj files, 18 bytes otherwise.
func (f *File) coffSymbolStride() int {
	if f.BigObjHeader != nil {
		return COFFSymbolBigObjSize
	}
	return COFFSymbolSize
}
