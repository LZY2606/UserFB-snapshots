// Package tests verifies the contract of the io.ReaderAt-based entry point
// (ParseELFReader) against the original byte-slice entry point, using the
// synthetic images from task_support/fixtures and a sparse image whose tables
// and contents live past the 4 GiB mark.
package tests

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"testing"

	er "github.com/yalue/elf_reader"
	"github.com/yalue/elf_reader/task_support/fixtures"
	"github.com/yalue/elf_reader/task_support/taskcheck"
)

// allImages runs body for every combination of word size and endianness.
func allImages(body func(wide, big bool, raw []byte)) {
	for _, wide := range []bool{false, true} {
		for _, big := range []bool{false, true} {
			body(wide, big, fixtures.Image(wide, big))
		}
	}
}

// parseBoth parses raw through both entry points, reporting failures as
// checks. It returns nil files if either parse failed.
func parseBoth(c *taskcheck.C, raw []byte) (er.ELFFile, er.ELFFile) {
	fromBytes, e := er.ParseELFFile(raw)
	c.Check(e == nil, "ParseELFFile failed: %s", e)
	fromReader, e := er.ParseELFReader(bytes.NewReader(raw), int64(len(raw)))
	c.Check(e == nil, "ParseELFReader failed: %s", e)
	if e != nil {
		return nil, nil
	}
	return fromBytes, fromReader
}

// TestReaderHeaders checks that the reader-based entry point detects the
// class and endianness, returns the same concrete types, and yields the same
// header, section header, program header, and section name data as the
// byte-slice path. It also checks a sparse 64-bit image whose tables and
// contents live past 4 GiB.
func TestReaderHeaders(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		allImages(func(wide, big bool, raw []byte) {
			label := fmt.Sprintf("wide=%v big=%v", wide, big)
			fromBytes, fromReader := parseBoth(c, raw)
			if fromReader == nil {
				return
			}
			// The concrete file type must match the byte-slice path.
			if wide {
				f, ok := fromReader.(*er.ELF64File)
				c.Check(ok, "%s: expected *ELF64File, got %T", label,
					fromReader)
				if ok {
					c.Check(f.Raw == nil,
						"%s: reader-based Raw must stay nil", label)
				}
			} else {
				f, ok := fromReader.(*er.ELF32File)
				c.Check(ok, "%s: expected *ELF32File, got %T", label,
					fromReader)
				if ok {
					c.Check(f.Raw == nil,
						"%s: reader-based Raw must stay nil", label)
				}
			}
			c.Check(fromReader.GetFileType() == fromBytes.GetFileType(),
				"%s: file type mismatch", label)
			c.Check(fromReader.GetMachineType() == fromBytes.GetMachineType(),
				"%s: machine type mismatch", label)
			c.Check(fromReader.GetSectionCount() == 8,
				"%s: expected 8 sections, got %d", label,
				fromReader.GetSectionCount())
			c.Check(fromReader.GetSegmentCount() == 1,
				"%s: expected 1 segment, got %d", label,
				fromReader.GetSegmentCount())
			// The null section has no name on either path.
			_, e0 := fromReader.GetSectionName(0)
			c.Check(e0 != nil, "%s: null section should have no name", label)
			for i := uint16(1); i < fromReader.GetSectionCount(); i++ {
				wantName, e := fromBytes.GetSectionName(i)
				c.Check(e == nil, "%s: section %d name (bytes): %s",
					label, i, e)
				gotName, e := fromReader.GetSectionName(i)
				c.Check(e == nil, "%s: section %d name (reader): %s",
					label, i, e)
				c.Check(gotName == wantName,
					"%s: section %d name %q != %q", label, i, gotName,
					wantName)
				wantHeader, e := fromBytes.GetSectionHeader(i)
				c.Check(e == nil, "%s: section %d header (bytes): %s",
					label, i, e)
				gotHeader, e := fromReader.GetSectionHeader(i)
				c.Check(e == nil, "%s: section %d header (reader): %s",
					label, i, e)
				if e != nil {
					continue
				}
				c.Check(gotHeader.GetType() == wantHeader.GetType() &&
					gotHeader.GetFileOffset() == wantHeader.GetFileOffset() &&
					gotHeader.GetSize() == wantHeader.GetSize(),
					"%s: section %d header mismatch", label, i)
			}
			// Section names come from the fixture's string table.
			name, e := fromReader.GetSectionName(3)
			c.Check(e == nil && name == ".symtab",
				"%s: section 3 name %q, %s", label, name, e)
			name, e = fromReader.GetSectionName(5)
			c.Check(e == nil && name == ".text",
				"%s: section 5 name %q, %s", label, name, e)
			// Program headers must agree, including the original offsets.
			wantPH, e := fromBytes.GetProgramHeader(0)
			c.Check(e == nil, "%s: program header (bytes): %s", label, e)
			gotPH, e := fromReader.GetProgramHeader(0)
			c.Check(e == nil, "%s: program header (reader): %s", label, e)
			if e == nil {
				c.Check(gotPH.GetFileOffset() == wantPH.GetFileOffset() &&
					gotPH.GetFileSize() == 4 && gotPH.GetMemorySize() == 20,
					"%s: program header fields mismatch", label)
			}
			// Section and segment contents must be identical across paths.
			for i := uint16(1); i < fromReader.GetSectionCount(); i++ {
				wantContent, e1 := fromBytes.GetSectionContent(i)
				gotContent, e2 := fromReader.GetSectionContent(i)
				c.Check((e1 == nil) == (e2 == nil),
					"%s: section %d content error mismatch: %s vs %s",
					label, i, e1, e2)
				if e1 == nil && e2 == nil {
					c.Check(bytes.Equal(wantContent, gotContent),
						"%s: section %d content mismatch", label, i)
				}
			}
			wantSeg, e := fromBytes.GetSegmentContent(0)
			c.Check(e == nil, "%s: segment content (bytes): %s", label, e)
			gotSeg, e := fromReader.GetSegmentContent(0)
			c.Check(e == nil, "%s: segment content (reader): %s", label, e)
			c.Check(bytes.Equal(wantSeg, gotSeg),
				"%s: segment content mismatch", label)
		})
		checkSparseImage(c)
	})
}

// sparseReader serves an image made of a few populated regions separated by
// huge holes. Any read touching a hole fails, so tests can prove the library
// never reads the gaps between tables and contents.
type sparseReader struct {
	size      int64
	regions   []sparseRegion
	totalRead int64
}

type sparseRegion struct {
	off  int64
	data []byte
}

func (s *sparseReader) ReadAt(p []byte, off int64) (int, error) {
	if off < 0 {
		return 0, fmt.Errorf("negative offset %d", off)
	}
	if off+int64(len(p)) > s.size {
		return 0, io.EOF
	}
	s.totalRead += int64(len(p))
	remaining := p
	pos := off
	for len(remaining) > 0 {
		found := false
		for i := range s.regions {
			r := &s.regions[i]
			if (pos >= r.off) && (pos < r.off+int64(len(r.data))) {
				n := int64(len(r.data)) - (pos - r.off)
				if n > int64(len(remaining)) {
					n = int64(len(remaining))
				}
				copy(remaining[:n], r.data[pos-r.off:pos-r.off+n])
				remaining = remaining[n:]
				pos += n
				found = true
				break
			}
		}
		if !found {
			return int(pos - off),
				fmt.Errorf("read from sparse hole at offset 0x%x", pos)
		}
	}
	return len(p), nil
}

// buildSparseImage builds a 64-bit little-endian ELF image whose section
// header table, .text contents, and string table all live past the 4 GiB
// mark. The returned reader fails any read that touches the sparse holes.
func buildSparseImage() *sparseReader {
	const (
		shOff   = (1 << 32) + 0x1000
		textOff = (1 << 32) + 0x2000
		strOff  = (1 << 32) + 0x2100
	)
	names := "\x00.text\x00.shstrtab\x00"
	text := []byte{0x90, 0x90, 0x90, 0xc3}

	var header [64]byte
	copy(header[:], "\x7fELF")
	header[4] = 2                                  // 64-bit class
	header[5] = 1                                  // little endian
	header[6] = 1                                  // version
	binary.LittleEndian.PutUint16(header[16:], 2)  // executable
	binary.LittleEndian.PutUint16(header[18:], 62) // AMD64
	binary.LittleEndian.PutUint32(header[20:], 1)  // version
	binary.LittleEndian.PutUint64(header[24:], 0x401000)
	binary.LittleEndian.PutUint64(header[32:], 0x40) // program headers
	binary.LittleEndian.PutUint64(header[40:], shOff)
	binary.LittleEndian.PutUint16(header[52:], 64) // header size
	binary.LittleEndian.PutUint16(header[54:], 56) // program header size
	binary.LittleEndian.PutUint16(header[56:], 1)  // program header count
	binary.LittleEndian.PutUint16(header[58:], 64) // section header size
	binary.LittleEndian.PutUint16(header[60:], 3)  // section header count
	binary.LittleEndian.PutUint16(header[62:], 2)  // section names table

	var ph [56]byte
	binary.LittleEndian.PutUint32(ph[0:], 1) // loadable segment
	binary.LittleEndian.PutUint32(ph[4:], 5) // readable, executable
	binary.LittleEndian.PutUint64(ph[8:], textOff)
	binary.LittleEndian.PutUint64(ph[16:], 0x401000)
	binary.LittleEndian.PutUint64(ph[24:], 0x401000)
	binary.LittleEndian.PutUint64(ph[32:], 4)  // bytes in the file
	binary.LittleEndian.PutUint64(ph[40:], 20) // bytes in memory
	binary.LittleEndian.PutUint64(ph[48:], 1)

	sh := make([]byte, 3*64)
	// Section 1: .text
	binary.LittleEndian.PutUint32(sh[64:], 1) // name offset
	binary.LittleEndian.PutUint32(sh[68:], 1) // bits section
	binary.LittleEndian.PutUint64(sh[72:], 6) // allocated, executable
	binary.LittleEndian.PutUint64(sh[80:], 0x401000)
	binary.LittleEndian.PutUint64(sh[88:], textOff)
	binary.LittleEndian.PutUint64(sh[96:], uint64(len(text)))
	binary.LittleEndian.PutUint64(sh[112:], 1)
	// Section 2: .shstrtab
	binary.LittleEndian.PutUint32(sh[128:], 7) // name offset
	binary.LittleEndian.PutUint32(sh[132:], 3) // string table
	binary.LittleEndian.PutUint64(sh[152:], strOff)
	binary.LittleEndian.PutUint64(sh[160:], uint64(len(names)))
	binary.LittleEndian.PutUint64(sh[176:], 1)

	head := make([]byte, 0, len(header)+len(ph))
	head = append(head, header[:]...)
	head = append(head, ph[:]...)
	return &sparseReader{
		size: strOff + int64(len(names)),
		regions: []sparseRegion{
			{0, head},
			{shOff, sh},
			{textOff, text},
			{strOff, []byte(names)},
		},
	}
}

// checkSparseImage verifies that tables and contents past the 4 GiB mark are
// addressed by their original 64-bit offsets, and that the library never
// reads the sparse holes in between.
func checkSparseImage(c *taskcheck.C) {
	source := buildSparseImage()
	f, e := er.ParseELFReader(source, source.size)
	c.Check(e == nil, "sparse image parse failed: %s", e)
	if e != nil {
		return
	}
	if _, ok := f.(*er.ELF64File); !ok {
		c.Check(false, "sparse image: expected *ELF64File, got %T", f)
		return
	}
	c.Check(f.GetSectionCount() == 3, "sparse image: expected 3 sections, "+
		"got %d", f.GetSectionCount())
	c.Check(f.GetSegmentCount() == 1, "sparse image: expected 1 segment, "+
		"got %d", f.GetSegmentCount())
	// The original offsets past 4 GiB must be preserved.
	section, e := f.GetSectionHeader(1)
	c.Check(e == nil, "sparse image: .text header: %s", e)
	if e == nil {
		c.Check(section.GetFileOffset() == (1<<32)+0x2000,
			"sparse image: .text offset 0x%x", section.GetFileOffset())
	}
	segment, e := f.GetProgramHeader(0)
	c.Check(e == nil, "sparse image: program header: %s", e)
	if e == nil {
		c.Check(segment.GetFileOffset() == (1<<32)+0x2000 &&
			segment.GetFileSize() == 4 && segment.GetMemorySize() == 20,
			"sparse image: segment fields mismatch")
	}
	name, e := f.GetSectionName(1)
	c.Check(e == nil && name == ".text", "sparse image: section 1 name %q, "+
		"%s", name, e)
	name, e = f.GetSectionName(2)
	c.Check(e == nil && name == ".shstrtab", "sparse image: section 2 name "+
		"%q, %s", name, e)
	content, e := f.GetSectionContent(1)
	c.Check(e == nil && bytes.Equal(content, []byte{0x90, 0x90, 0x90, 0xc3}),
		"sparse image: .text content %x, %s", content, e)
	// Segments are limited to FileSize; the MemorySize zero-fill is not
	// file content.
	seg, e := f.GetSegmentContent(0)
	c.Check(e == nil && len(seg) == 4 && seg[3] == 0xc3,
		"sparse image: segment content %x, %s", seg, e)
	// The sparse reader fails on hole reads, so reaching this point already
	// proves no hole was touched. Also bound the total amount read: the
	// whole image must never be pulled in.
	c.Check(source.totalRead < 4096,
		"sparse image: read %d bytes, expected only tables and contents",
		source.totalRead)
}

// TestReaderSymbols checks that symbol tables, relocations, dynamic entries,
// and string tables read through the reader match the byte-slice path, and
// that caller mutations of returned content can't poison later queries.
func TestReaderSymbols(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		allImages(func(wide, big bool, raw []byte) {
			label := fmt.Sprintf("wide=%v big=%v", wide, big)
			fromBytes, fromReader := parseBoth(c, raw)
			if fromReader == nil {
				return
			}
			c.Check(fromReader.IsSymbolTable(3),
				"%s: section 3 should be a symbol table", label)
			symbols, names, e := fromReader.GetSymbols(3)
			c.Check(e == nil, "%s: GetSymbols: %s", label, e)
			if e != nil {
				return
			}
			c.Check(len(symbols) == 2 && len(names) == 2,
				"%s: expected 2 symbols, got %d", label, len(symbols))
			c.Check(names[1] == "entry", "%s: symbol 1 name %q", label,
				names[1])
			c.Check(symbols[1].GetValue() == 0x401000,
				"%s: symbol 1 value 0x%x", label, symbols[1].GetValue())
			c.Check(symbols[1].GetSize() == 4, "%s: symbol 1 size %d",
				label, symbols[1].GetSize())
			c.Check(symbols[1].GetSectionIndex() == 5,
				"%s: symbol 1 section %d", label,
				symbols[1].GetSectionIndex())
			// Symbol indices and values must match the byte-slice path.
			wantSymbols, wantNames, e := fromBytes.GetSymbols(3)
			c.Check(e == nil, "%s: GetSymbols (bytes): %s", label, e)
			if e == nil && len(wantSymbols) == len(symbols) {
				for i := range symbols {
					c.Check(symbols[i].GetName() == wantSymbols[i].GetName() &&
						symbols[i].GetValue() == wantSymbols[i].GetValue() &&
						symbols[i].GetSize() == wantSymbols[i].GetSize() &&
						names[i] == wantNames[i],
						"%s: symbol %d mismatch", label, i)
				}
			}
			// Relocations and their symbol associations.
			c.Check(fromReader.IsRelocationTable(4),
				"%s: section 4 should be a relocation table", label)
			relocations, e := fromReader.GetRelocations(4)
			c.Check(e == nil, "%s: GetRelocations: %s", label, e)
			if e == nil {
				c.Check(len(relocations) == 1,
					"%s: expected 1 relocation, got %d", label,
					len(relocations))
				r := relocations[0]
				c.Check(r.SymbolIndex() == 1 && r.Type() == 1,
					"%s: relocation symbol %d type %d", label,
					r.SymbolIndex(), r.Type())
				c.Check(r.Offset() == 0x401000 && r.Addend() == 5,
					"%s: relocation offset 0x%x addend %d", label,
					r.Offset(), r.Addend())
				wantRelocations, e := fromBytes.GetRelocations(4)
				c.Check(e == nil, "%s: GetRelocations (bytes): %s", label, e)
				if e == nil && len(wantRelocations) == len(relocations) {
					c.Check(r.Offset() == wantRelocations[0].Offset() &&
						r.SymbolIndex() == wantRelocations[0].SymbolIndex() &&
						r.Type() == wantRelocations[0].Type() &&
						r.Addend() == wantRelocations[0].Addend(),
						"%s: relocation mismatch across paths", label)
				}
			}
			// Dynamic entries.
			c.Check(fromReader.IsDynamicSection(6),
				"%s: section 6 should be a dynamic section", label)
			dynamic, e := fromReader.DynamicEntries(6)
			c.Check(e == nil, "%s: DynamicEntries: %s", label, e)
			if e == nil {
				c.Check(len(dynamic) == 2,
					"%s: expected 2 dynamic entries, got %d", label,
					len(dynamic))
				c.Check(dynamic[0].GetTag().GetValue() == 5 &&
					dynamic[0].GetValue() == 0x402000,
					"%s: dynamic entry 0 mismatch", label)
				c.Check(dynamic[1].GetTag().GetValue() == 0,
					"%s: dynamic entry 1 should be the terminator", label)
			}
			// String tables.
			strings, e := fromReader.GetStringTable(2)
			c.Check(e == nil, "%s: GetStringTable: %s", label, e)
			if e == nil {
				c.Check(len(strings) == 2 && strings[1] == "entry",
					"%s: string table %q", label, strings)
			}
		})
		// Mutating content returned by one call must not affect later
		// queries: every result is an independent copy.
		raw := fixtures.Image(true, false)
		f, e := er.ParseELFReader(bytes.NewReader(raw), int64(len(raw)))
		c.Check(e == nil, "mutation test parse: %s", e)
		if e != nil {
			return
		}
		content, e := f.GetSectionContent(3)
		c.Check(e == nil, "mutation test .symtab read: %s", e)
		if e == nil {
			for i := range content {
				content[i] = 0xff
			}
		}
		nameTable, e := f.GetSectionContent(2)
		c.Check(e == nil, "mutation test .strtab read: %s", e)
		if e == nil && len(nameTable) > 1 {
			nameTable[1] = 'X'
		}
		symbols, names, e := f.GetSymbols(3)
		c.Check(e == nil, "mutation test GetSymbols: %s", e)
		if e == nil {
			c.Check(names[1] == "entry" && symbols[1].GetValue() == 0x401000,
				"mutation test: symbol resolution was polluted")
		}
	})
}

// recordingReader wraps an io.ReaderAt and records every read issued.
type recordingReader struct {
	r     io.ReaderAt
	reads []readRecord
}

type readRecord struct {
	off int64
	n   int64
}

func (r *recordingReader) ReadAt(p []byte, off int64) (int, error) {
	r.reads = append(r.reads, readRecord{off, int64(len(p))})
	return r.r.ReadAt(p, off)
}

// failAtReader fails reads at or past a configurable offset with a sentinel
// error, simulating a misbehaving source.
type failAtReader struct {
	r       io.ReaderAt
	failOff int64
	err     error
}

func (f *failAtReader) ReadAt(p []byte, off int64) (int, error) {
	if (f.failOff >= 0) && (off >= f.failOff) {
		return 0, f.err
	}
	return f.r.ReadAt(p, off)
}

// closeTrackingReader records whether Close was ever called.
type closeTrackingReader struct {
	io.ReaderAt
	closed bool
}

func (c *closeTrackingReader) Close() error {
	c.closed = true
	return nil
}

// TestReaderIO checks the I/O behavior of the reader-based path: NOBITS
// errors, FileSize-limited segments, zero-length reads at the legal end of
// the file, independent result buffers, wrapped short-read and source
// errors, no failure caching, and that the source is never closed.
func TestReaderIO(t *testing.T) {
	taskcheck.Run(t, func(c *taskcheck.C) {
		allImages(func(wide, big bool, raw []byte) {
			label := fmt.Sprintf("wide=%v big=%v", wide, big)
			f, e := er.ParseELFReader(bytes.NewReader(raw), int64(len(raw)))
			c.Check(e == nil, "%s: parse failed: %s", label, e)
			if e != nil {
				return
			}
			// NOBITS sections keep returning the dedicated error type.
			_, e = f.GetSectionContent(7)
			var nobits er.UninitializedDataSectionError
			c.Check(errors.As(e, &nobits),
				"%s: expected UninitializedDataSectionError, got %s",
				label, e)
			c.Check(uint16(nobits) == 7,
				"%s: NOBITS error index %d", label, uint16(nobits))
			// Segments return FileSize bytes; MemorySize zero-fill is not
			// file content.
			segment, e := f.GetSegmentContent(0)
			c.Check(e == nil, "%s: segment content: %s", label, e)
			c.Check(len(segment) == 4 && segment[3] == 0xc3,
				"%s: segment content %x", label, segment)
			// The zero-length null section succeeds without any read.
			content, e := f.GetSectionContent(0)
			c.Check(e == nil && len(content) == 0,
				"%s: null section content len %d, %s", label,
				len(content), e)
			// Each successful call returns an independent buffer.
			first, e := f.GetSectionContent(5)
			c.Check(e == nil, "%s: .text read: %s", label, e)
			if e == nil && len(first) > 0 {
				first[0] = 0xcc
				second, e := f.GetSectionContent(5)
				c.Check(e == nil, "%s: .text re-read: %s", label, e)
				c.Check(len(second) > 0 && second[0] == 0x90,
					"%s: re-read observed caller mutation", label)
			}
		})

		// A zero-length section located exactly at the legal end of the
		// file must succeed without issuing any read there.
		raw := fixtures.Image(true, false)
		shdr := 0x100 + 5*64 // .text section header in the 64-bit fixture
		binary.LittleEndian.PutUint64(raw[shdr+24:], uint64(len(raw)))
		binary.LittleEndian.PutUint64(raw[shdr+32:], 0)
		recorder := &recordingReader{r: bytes.NewReader(raw)}
		f, e := er.ParseELFReader(recorder, int64(len(raw)))
		c.Check(e == nil, "zero-length section parse: %s", e)
		if e == nil {
			content, e := f.GetSectionContent(5)
			c.Check(e == nil && len(content) == 0,
				"zero-length section at EOF: len %d, %s", len(content), e)
			readAtEnd := false
			for _, r := range recorder.reads {
				if r.off >= int64(len(raw)) {
					readAtEnd = true
				}
			}
			c.Check(!readAtEnd,
				"zero-length content issued a read at/past end of file")
		}

		// A truncated source must fail parsing with an errors.Is-visible
		// cause, not partial data.
		raw = fixtures.Image(true, false)
		truncated := raw[:0x180] // cuts the section header table short
		_, e = er.ParseELFReader(bytes.NewReader(truncated),
			int64(len(truncated)))
		c.Check(e != nil, "truncated image should fail to parse")
		c.Check(errors.Is(e, io.EOF) || errors.Is(e, io.ErrUnexpectedEOF),
			"truncated image error should wrap EOF: %s", e)

		// A source error must surface through errors.Is, partial data must
		// not be treated as success, and failures must not be cached.
		errSource := errors.New("source failure")
		failing := &failAtReader{
			r:       bytes.NewReader(raw),
			failOff: 0x600, // the .text contents in the fixture
			err:     errSource,
		}
		f, e = er.ParseELFReader(failing, int64(len(raw)))
		c.Check(e == nil, "parse over failing reader: %s", e)
		if e == nil {
			_, e = f.GetSectionContent(5)
			c.Check(errors.Is(e, errSource),
				"expected wrapped source error, got %s", e)
			// The failure isn't cached: once the source recovers, the
			// same call succeeds.
			failing.failOff = -1
			content, e := f.GetSectionContent(5)
			c.Check(e == nil && len(content) == 4 && content[3] == 0xc3,
				"read after recovery: %x, %s", content, e)
		}

		// The library never closes the caller's reader.
		tracked := &closeTrackingReader{ReaderAt: bytes.NewReader(raw)}
		f, e = er.ParseELFReader(tracked, int64(len(raw)))
		c.Check(e == nil, "parse over close-tracking reader: %s", e)
		if e == nil {
			_, e = f.GetSectionContent(5)
			c.Check(e == nil, "read over close-tracking reader: %s", e)
			_, _, e = f.GetSymbols(3)
			c.Check(e == nil, "symbols over close-tracking reader: %s", e)
		}
		c.Check(!tracked.closed, "library closed the caller's reader")

		// The byte-slice path keeps its Raw aliasing behavior.
		raw = fixtures.Image(true, false)
		fromBytes, e := er.ParseELFFile(raw)
		c.Check(e == nil, "raw-path parse: %s", e)
		if e == nil {
			content, e := fromBytes.GetSectionContent(5)
			c.Check(e == nil, "raw-path .text: %s", e)
			if e == nil && len(content) > 0 {
				content[0] = 0xcc
				c.Check(raw[0x600] == 0xcc,
					"raw-path content no longer aliases Raw")
			}
		}
	})
}
