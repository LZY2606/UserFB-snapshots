package elf_reader

// This file contains an io.ReaderAt-based entry point for parsing ELF files,
// intended for offline inspection of large files where loading the entire
// image into memory is undesirable. The parser only reads the identification
// bytes, the ELF header, and the section/program header tables up front.
// Section names, symbol tables, relocations, dynamic entries, and section or
// segment contents are read lazily, on each call, using the original file
// offsets recorded in the tables. The source is never read beyond the ranges
// being requested, so sparse holes between tables and contents are never
// touched. The reader remains owned by the caller and is never closed.

import (
	"encoding/binary"
	"fmt"
	"io"
)

// Parses an ELF file from the given io.ReaderAt, which must contain exactly
// size bytes of file data. The class (32- or 64-bit) and endianness are
// detected from the file's identification bytes, and the returned ELFFile is
// either an *ELF64File or *ELF32File, just as with ParseELFFile.
//
// Unlike ParseELFFile, the file's Raw field remains nil, and contents are
// read from r on demand: every call to GetSectionName, GetSectionContent,
// GetSegmentContent, GetSymbols, GetRelocations, DynamicEntries, and the
// other query methods reads only the byte ranges it needs, at the original
// file offsets from the section and program headers. Each successful call
// returns a freshly-read copy, so modifying a returned slice never affects
// subsequent calls. Read failures (including short reads) are returned as
// errors wrapping the underlying cause (detectable with errors.Is), and
// nothing is cached between calls, so a later call may succeed after an
// earlier one failed. The caller retains ownership of r; this library never
// closes it.
func ParseELFReader(r io.ReaderAt, size int64) (ELFFile, error) {
	if r == nil {
		return nil, fmt.Errorf("Cannot parse a nil reader")
	}
	if size < 5 {
		return nil, fmt.Errorf("Invalid ELF file: is only %d bytes", size)
	}
	var classByte [1]byte
	_, e := io.ReadFull(io.NewSectionReader(r, 4, 1), classByte[:])
	if e != nil {
		return nil, fmt.Errorf("Failed reading ELF class: %w", e)
	}
	if classByte[0] == 2 {
		return parseELF64Reader(r, size)
	}
	return parseELF32Reader(r, size)
}

// Returns the total size of the underlying file data, whether the file was
// parsed from a byte slice or an io.ReaderAt.
func (f *ELF64File) dataSize() uint64 {
	if f.reader != nil {
		return uint64(f.readerSize)
	}
	return uint64(len(f.Raw))
}

// Returns length bytes of file data starting at offset. The offset must have
// already been bounds-checked against dataSize(). When the file was parsed
// from a byte slice, the result aliases Raw. When parsed from an io.ReaderAt,
// a freshly-read copy is returned, and zero-length reads don't touch the
// source at all.
func (f *ELF64File) readAt(offset, length uint64) ([]byte, error) {
	if f.reader == nil {
		return f.Raw[offset : offset+length], nil
	}
	return readFileRange(f.reader, offset, length)
}

// Returns the total size of the underlying file data, whether the file was
// parsed from a byte slice or an io.ReaderAt.
func (f *ELF32File) dataSize() uint64 {
	if f.reader != nil {
		return uint64(f.readerSize)
	}
	return uint64(len(f.Raw))
}

// Returns length bytes of file data starting at offset. Behaves like the
// ELF64File readAt method.
func (f *ELF32File) readAt(offset, length uint64) ([]byte, error) {
	if f.reader == nil {
		return f.Raw[offset : offset+length], nil
	}
	return readFileRange(f.reader, offset, length)
}

// Reads exactly length bytes from r at the given offset, returning them in a
// newly-allocated slice. Zero-length reads succeed without touching r. Short
// reads and source errors are returned wrapped, so callers can use errors.Is
// to check for causes such as io.EOF or io.ErrUnexpectedEOF.
func readFileRange(r io.ReaderAt, offset, length uint64) ([]byte, error) {
	if length == 0 {
		return []byte{}, nil
	}
	toReturn := make([]byte, length)
	_, e := io.ReadFull(io.NewSectionReader(r, int64(offset), int64(length)),
		toReturn)
	if e != nil {
		return nil, fmt.Errorf("Failed reading %d bytes at offset 0x%x: %w",
			length, offset, e)
	}
	return toReturn, nil
}

// Reads the 6 identification bytes at the start of the file, returning the
// (always little-endian) signature and the file's endianness.
func readELFIdent(r io.ReaderAt) (uint32, binary.ByteOrder, error) {
	var ident [6]byte
	_, e := io.ReadFull(io.NewSectionReader(r, 0, 6), ident[:])
	if e != nil {
		return 0, nil, fmt.Errorf("Failed reading ELF signature: %w", e)
	}
	signature := binary.LittleEndian.Uint32(ident[:4])
	var endianness binary.ByteOrder
	if ident[5] != 1 {
		if ident[5] != 2 {
			return 0, nil, fmt.Errorf("Invalid encoding/endianness: %d",
				ident[5])
		}
		endianness = binary.BigEndian
	} else {
		endianness = binary.LittleEndian
	}
	return signature, endianness, nil
}

// Attempts to parse a 64-bit ELF file from the given reader, reading only the
// header and the section/program header tables.
func parseELF64Reader(r io.ReaderAt, size int64) (*ELF64File, error) {
	toReturn := ELF64File{
		reader:     r,
		readerSize: size,
	}
	signature, endianness, e := readELFIdent(r)
	if e != nil {
		return nil, e
	}
	var header ELF64Header
	e = binary.Read(io.NewSectionReader(r, 0, size), endianness, &header)
	if e != nil {
		return nil, fmt.Errorf("Failed reading ELF64 header: %w", e)
	}
	// The signature may have been incorrectly read backwards if we're reading
	// a big-endian ELF, so copy the correct little-endian version.
	header.Signature = signature
	if header.Class != 2 {
		return nil, fmt.Errorf("ELF class incorrect for 64-bit: %d",
			header.Class)
	}
	toReturn.Header = header
	toReturn.Endianness = endianness
	if e = toReturn.parseProgramHeadersFromReader(); e != nil {
		return nil, e
	}
	if e = toReturn.parseSectionHeadersFromReader(); e != nil {
		return nil, e
	}
	return &toReturn, nil
}

// Used during reader-based initialization to fill in the Segments slice.
func (f *ELF64File) parseProgramHeadersFromReader() error {
	offset := f.Header.ProgramHeaderOffset
	if offset >= uint64(f.readerSize) {
		return fmt.Errorf("Invalid program header offset: 0x%x", offset)
	}
	data := io.NewSectionReader(f.reader, int64(offset),
		f.readerSize-int64(offset))
	segments := make([]ELF64ProgramHeader, f.Header.ProgramHeaderEntries)
	e := binary.Read(data, f.Endianness, segments)
	if e != nil {
		return fmt.Errorf("Failed reading program header table: %w", e)
	}
	f.Segments = segments
	return nil
}

// Used during reader-based initialization to fill in the Sections slice.
func (f *ELF64File) parseSectionHeadersFromReader() error {
	// Don't require a valid section header offset if there are no sections.
	if f.Header.SectionHeaderEntries == 0 {
		f.Sections = nil
		return nil
	}
	offset := f.Header.SectionHeaderOffset
	if offset >= uint64(f.readerSize) {
		return fmt.Errorf("Invalid section header offset: 0x%x", offset)
	}
	data := io.NewSectionReader(f.reader, int64(offset),
		f.readerSize-int64(offset))
	sections := make([]ELF64SectionHeader, f.Header.SectionHeaderEntries)
	e := binary.Read(data, f.Endianness, sections)
	if e != nil {
		return fmt.Errorf("Failed reading section header table: %w", e)
	}
	f.Sections = sections
	return nil
}

// Attempts to parse a 32-bit ELF file from the given reader, reading only the
// header and the section/program header tables.
func parseELF32Reader(r io.ReaderAt, size int64) (*ELF32File, error) {
	toReturn := ELF32File{
		reader:     r,
		readerSize: size,
	}
	signature, endianness, e := readELFIdent(r)
	if e != nil {
		return nil, e
	}
	if signature != 0x464c457f {
		return nil, fmt.Errorf("Invalid ELF signature: 0x%08x", signature)
	}
	var header ELF32Header
	e = binary.Read(io.NewSectionReader(r, 0, size), endianness, &header)
	if e != nil {
		return nil, fmt.Errorf("Failed reading ELF32 header: %w", e)
	}
	// This may have been incorrectly reversed if we're big-endian, so copy
	// the correct little-endian version just in case.
	header.Signature = signature
	if header.Class != 1 {
		return nil, fmt.Errorf("ELF class incorrect for 32-bit: %d",
			header.Class)
	}
	toReturn.Header = header
	toReturn.Endianness = endianness
	if e = toReturn.parseProgramHeadersFromReader(); e != nil {
		return nil, e
	}
	if e = toReturn.parseSectionHeadersFromReader(); e != nil {
		return nil, e
	}
	return &toReturn, nil
}

// Used during reader-based initialization to fill in the Segments slice.
func (f *ELF32File) parseProgramHeadersFromReader() error {
	offset := f.Header.ProgramHeaderOffset
	if uint64(offset) >= uint64(f.readerSize) {
		return fmt.Errorf("Invalid program header offset: 0x%x", offset)
	}
	data := io.NewSectionReader(f.reader, int64(offset),
		f.readerSize-int64(offset))
	segments := make([]ELF32ProgramHeader, f.Header.ProgramHeaderEntries)
	e := binary.Read(data, f.Endianness, segments)
	if e != nil {
		return fmt.Errorf("Failed reading program header table: %w", e)
	}
	f.Segments = segments
	return nil
}

// Used during reader-based initialization to fill in the Sections slice.
func (f *ELF32File) parseSectionHeadersFromReader() error {
	// Don't require a valid section header offset if there are no sections.
	if f.Header.SectionHeaderEntries == 0 {
		f.Sections = nil
		return nil
	}
	offset := f.Header.SectionHeaderOffset
	if uint64(offset) >= uint64(f.readerSize) {
		return fmt.Errorf("Invalid section header offset: 0x%x", offset)
	}
	data := io.NewSectionReader(f.reader, int64(offset),
		f.readerSize-int64(offset))
	sections := make([]ELF32SectionHeader, f.Header.SectionHeaderEntries)
	e := binary.Read(data, f.Endianness, sections)
	if e != nil {
		return fmt.Errorf("Failed reading section header table: %w", e)
	}
	f.Sections = sections
	return nil
}
