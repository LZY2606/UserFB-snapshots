// Copyright 2009 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

type DosHeader struct {
	MZSignature              uint16
	UsedBytesInTheLastPage   uint16
	FileSizeInPages          uint16
	NumberOfRelocationItems  uint16
	HeaderSizeInParagraphs   uint16
	MinimumExtraParagraphs   uint16
	MaximumExtraParagraphs   uint16
	InitialRelativeSS        uint16
	InitialSP                uint16
	CheckSum                 uint16
	InitialIP                uint16
	InitialRelativeCS        uint16
	AddressOfRelocationTable uint16
	OverlayNumber            uint16
	Reserved                 [4]uint16
	OEMid                    uint16
	OEMinfo                  uint16
	Reserved2                [10]uint16
	AddressOfNewExeHeader    uint32
}

type FileHeader struct {
	Machine              uint16
	NumberOfSections     uint16
	TimeDateStamp        uint32
	PointerToSymbolTable uint32
	NumberOfSymbols      uint32
	SizeOfOptionalHeader uint16
	Characteristics      uint16
}

// BigObjHeader carries the fields of the 56-byte ANLONI BigObj v2 object
// header that are not represented by FileHeader. It is only populated for
// BigObj files; ordinary COFF objects and PE images leave it nil. The
// standard 20-byte FileHeader is still populated from the corresponding
// BigObj fields, so existing header-based code keeps working.
type BigObjHeader struct {
	Sig1             uint16
	Sig2             uint16
	Version          uint16
	ClassID          [16]byte
	SizeOfData       uint32
	Flags            uint32
	MetaDataSize     uint32
	MetaDataOffset   uint32
	NumberOfSections uint32
}

const (
	// bigObjHeaderSize is the on-disk size of a BigObj v2 header.
	bigObjHeaderSize = 56
	// bigObjSig1 and bigObjSig2 open both BigObj and short import objects.
	bigObjSig1 = 0x0000
	bigObjSig2 = 0xffff
	// bigObjVersion2 is the only BigObj version understood by this package.
	bigObjVersion2 = 2
	// bigObjSymbolSize is the stride of a BigObj symbol slot: an ordinary
	// 18-byte COFFSymbol with SectionNumber widened from int16 to int32.
	bigObjSymbolSize = 20
)

type DataDirectory struct {
	VirtualAddress uint32
	Size           uint32
}

type OptionalHeader32 struct {
	Magic                       uint16
	MajorLinkerVersion          uint8
	MinorLinkerVersion          uint8
	SizeOfCode                  uint32
	SizeOfInitializedData       uint32
	SizeOfUninitializedData     uint32
	AddressOfEntryPoint         uint32
	BaseOfCode                  uint32
	BaseOfData                  uint32
	ImageBase                   uint32
	SectionAlignment            uint32
	FileAlignment               uint32
	MajorOperatingSystemVersion uint16
	MinorOperatingSystemVersion uint16
	MajorImageVersion           uint16
	MinorImageVersion           uint16
	MajorSubsystemVersion       uint16
	MinorSubsystemVersion       uint16
	Win32VersionValue           uint32
	SizeOfImage                 uint32
	SizeOfHeaders               uint32
	CheckSum                    uint32
	Subsystem                   uint16
	DllCharacteristics          uint16
	SizeOfStackReserve          uint32
	SizeOfStackCommit           uint32
	SizeOfHeapReserve           uint32
	SizeOfHeapCommit            uint32
	LoaderFlags                 uint32
	NumberOfRvaAndSizes         uint32
	DataDirectory               [16]DataDirectory
}

type OptionalHeader64 struct {
	Magic                       uint16
	MajorLinkerVersion          uint8
	MinorLinkerVersion          uint8
	SizeOfCode                  uint32
	SizeOfInitializedData       uint32
	SizeOfUninitializedData     uint32
	AddressOfEntryPoint         uint32
	BaseOfCode                  uint32
	ImageBase                   uint64
	SectionAlignment            uint32
	FileAlignment               uint32
	MajorOperatingSystemVersion uint16
	MinorOperatingSystemVersion uint16
	MajorImageVersion           uint16
	MinorImageVersion           uint16
	MajorSubsystemVersion       uint16
	MinorSubsystemVersion       uint16
	Win32VersionValue           uint32
	SizeOfImage                 uint32
	SizeOfHeaders               uint32
	CheckSum                    uint32
	Subsystem                   uint16
	DllCharacteristics          uint16
	SizeOfStackReserve          uint64
	SizeOfStackCommit           uint64
	SizeOfHeapReserve           uint64
	SizeOfHeapCommit            uint64
	LoaderFlags                 uint32
	NumberOfRvaAndSizes         uint32
	DataDirectory               [16]DataDirectory
}

const (
	IMAGE_FILE_MACHINE_UNKNOWN   = 0x0
	IMAGE_FILE_MACHINE_AM33      = 0x1d3
	IMAGE_FILE_MACHINE_AMD64     = 0x8664
	IMAGE_FILE_MACHINE_ARM       = 0x1c0
	IMAGE_FILE_MACHINE_ARMNT     = 0x1c4
	IMAGE_FILE_MACHINE_ARM64     = 0xaa64
	IMAGE_FILE_MACHINE_EBC       = 0xebc
	IMAGE_FILE_MACHINE_I386      = 0x14c
	IMAGE_FILE_MACHINE_IA64      = 0x200
	IMAGE_FILE_MACHINE_M32R      = 0x9041
	IMAGE_FILE_MACHINE_MIPS16    = 0x266
	IMAGE_FILE_MACHINE_MIPSFPU   = 0x366
	IMAGE_FILE_MACHINE_MIPSFPU16 = 0x466
	IMAGE_FILE_MACHINE_POWERPC   = 0x1f0
	IMAGE_FILE_MACHINE_POWERPCFP = 0x1f1
	IMAGE_FILE_MACHINE_R4000     = 0x166
	IMAGE_FILE_MACHINE_SH3       = 0x1a2
	IMAGE_FILE_MACHINE_SH3DSP    = 0x1a3
	IMAGE_FILE_MACHINE_SH4       = 0x1a6
	IMAGE_FILE_MACHINE_SH5       = 0x1a8
	IMAGE_FILE_MACHINE_THUMB     = 0x1c2
	IMAGE_FILE_MACHINE_WCEMIPSV2 = 0x169
)

// IMAGE_DIRECTORY_ENTRY constants
const (
	IMAGE_DIRECTORY_ENTRY_EXPORT         = 0
	IMAGE_DIRECTORY_ENTRY_IMPORT         = 1
	IMAGE_DIRECTORY_ENTRY_RESOURCE       = 2
	IMAGE_DIRECTORY_ENTRY_EXCEPTION      = 3
	IMAGE_DIRECTORY_ENTRY_SECURITY       = 4
	IMAGE_DIRECTORY_ENTRY_BASERELOC      = 5
	IMAGE_DIRECTORY_ENTRY_DEBUG          = 6
	IMAGE_DIRECTORY_ENTRY_ARCHITECTURE   = 7
	IMAGE_DIRECTORY_ENTRY_GLOBALPTR      = 8
	IMAGE_DIRECTORY_ENTRY_TLS            = 9
	IMAGE_DIRECTORY_ENTRY_LOAD_CONFIG    = 10
	IMAGE_DIRECTORY_ENTRY_BOUND_IMPORT   = 11
	IMAGE_DIRECTORY_ENTRY_IAT            = 12
	IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT   = 13
	IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR = 14
)
