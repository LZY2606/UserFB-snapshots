// Copyright 2026 The Go Authors. All rights reserved.
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file.

package pe

import (
	"encoding/binary"
	"fmt"
	"io"
)

// newBigObjFile decodes a BigObj v2 object from r. Only the AMD64 machine
// type is currently supported. Reading stops at interpreting disk layout:
// section data, relocations and symbols are exposed as records, never
// executed or applied.
func newBigObjFile(r io.ReaderAt) (*File, error) {
	hdr := make([]byte, bigObjHeaderSize)
	if _, err := io.ReadFull(io.NewSectionReader(r, 0, bigObjHeaderSize), hdr); err != nil {
		return nil, fmt.Errorf("pe: fail to read bigobj header: %v", err)
	}
	if binary.LittleEndian.Uint16(hdr[0:2]) != bigObjSig1 ||
		binary.LittleEndian.Uint16(hdr[2:4]) != bigObjSig2 {
		return nil, fmt.Errorf("pe: bad bigobj signature")
	}
	version := binary.LittleEndian.Uint16(hdr[4:6])
	if version != bigObjVersion2 {
		return nil, fmt.Errorf("pe: unsupported bigobj version %d", version)
	}
	machine := binary.LittleEndian.Uint16(hdr[6:8])
	if machine != IMAGE_FILE_MACHINE_AMD64 {
		return nil, fmt.Errorf("pe: bigobj machine 0x%x is not supported; only AMD64 (0x%x)", machine, IMAGE_FILE_MACHINE_AMD64)
	}

	bo := &BigObjHeader{
		Sig1:             binary.LittleEndian.Uint16(hdr[0:2]),
		Sig2:             binary.LittleEndian.Uint16(hdr[2:4]),
		Version:          version,
		SizeOfData:       binary.LittleEndian.Uint32(hdr[28:32]),
		Flags:            binary.LittleEndian.Uint32(hdr[32:36]),
		MetaDataSize:     binary.LittleEndian.Uint32(hdr[36:40]),
		MetaDataOffset:   binary.LittleEndian.Uint32(hdr[40:44]),
		NumberOfSections: binary.LittleEndian.Uint32(hdr[44:48]),
	}
	copy(bo.ClassID[:], hdr[12:28])

	pointerToSymbolTable := binary.LittleEndian.Uint32(hdr[48:52])
	numberOfSymbols := binary.LittleEndian.Uint32(hdr[52:56])

	f := new(File)
	f.readerAt = r
	f.bigObj = true
	f.BigObjHeader = bo
	f.symbolStride = bigObjSymbolSize
	f.sectionCount32 = bo.NumberOfSections
	// BigObj has no standard 20-byte header on disk. The synthesized
	// FileHeader mirrors the fields that still fit; NumberOfSections stays
	// zero because its real value is 32-bit and must not be truncated.
	// SectionCount() is the authoritative section count.
	f.FileHeader = FileHeader{
		Machine:              machine,
		NumberOfSections:     0,
		TimeDateStamp:        binary.LittleEndian.Uint32(hdr[8:12]),
		PointerToSymbolTable: pointerToSymbolTable,
		NumberOfSymbols:      numberOfSymbols,
	}

	sr := io.NewSectionReader(r, 0, 1<<63-1)

	// Read string table first: long section and symbol names resolve
	// against it, and its location depends on the 20-byte slot stride.
	var err error
	f.StringTable, err = readStringTableStride(&f.FileHeader, bigObjSymbolSize, sr)
	if err != nil {
		return nil, err
	}

	// Read the 20-byte symbol slots, including every auxiliary record.
	f.COFFSymbols, err = readBigObjCOFFSymbols(&f.FileHeader, r)
	if err != nil {
		return nil, err
	}
	f.auxSlot = markAuxSlots(f.COFFSymbols)
	f.Symbols, err = removeAuxSymbols(f.COFFSymbols, f.StringTable)
	if err != nil {
		return nil, err
	}

	// The section table starts immediately after the 56-byte header and
	// uses the same 40-byte section headers as ordinary COFF objects.
	f.Sections = make([]*Section, bo.NumberOfSections)
	for i := uint32(0); i < bo.NumberOfSections; i++ {
		off := int64(bigObjHeaderSize) + int64(i)*int64(binary.Size(SectionHeader32{}))
		sh := new(SectionHeader32)
		if _, err := sr.Seek(off, seekStart); err != nil {
			return nil, err
		}
		if err := binary.Read(sr, binary.LittleEndian, sh); err != nil {
			return nil, fmt.Errorf("pe: fail to read bigobj section header %d: %v", i, err)
		}
		name, err := sh.fullName(f.StringTable)
		if err != nil {
			return nil, err
		}
		s := new(Section)
		s.SectionHeader = SectionHeader{
			Name:                 name,
			OriginalName:         sh.Name,
			VirtualSize:          sh.VirtualSize,
			VirtualAddress:       sh.VirtualAddress,
			Size:                 sh.SizeOfRawData,
			Offset:               sh.PointerToRawData,
			PointerToRelocations: sh.PointerToRelocations,
			PointerToLineNumbers: sh.PointerToLineNumbers,
			NumberOfRelocations:  sh.NumberOfRelocations,
			NumberOfLineNumbers:  sh.NumberOfLineNumbers,
			Characteristics:      sh.Characteristics,
		}
		r2 := io.ReaderAt(r)
		if sh.PointerToRawData == 0 { // .bss-style sections have no raw bytes
			r2 = zeroReaderAt{}
		}
		s.sr = io.NewSectionReader(r2, int64(s.SectionHeader.Offset), int64(s.SectionHeader.Size))
		s.ReaderAt = s.sr
		f.Sections[i] = s
	}
	for i := range f.Sections {
		f.Sections[i].Relocs, err = readRelocs(&f.Sections[i].SectionHeader, sr)
		if err != nil {
			return nil, err
		}
	}

	return f, nil
}
