ELF Reader
==========

About
-----

This library is for reading ELF files using the Go programming language. Go's
standard library already includes ELF-related functions, but these do not
include some useful functionality for displaying or accessing some aspects of
ELF files out-of-the-box.

This library supports both big and little-endian 32- and 64-bit ELF files. You
can specifically read 32-bit files by calling `ParseELF32File(...)`, or 64-bit
files by `ParseELF64File(...)`, or either type of file using
`ParseELFFile(...)`. If you use the generic `ParseELFFile(...)` function, then
you can either use the returned `ELFFile` interface directly, or use type
assertions to retrieve a 32-bit `*ELF32File` or a 64-bit `*ELF64File`.

Usage
-----

The following example shows how this library can be used to output section
names to standard output. For a more complete example of how to read
information from an ELF file, see the command-line tool at
`elf_view/elf_view.go`.

```go
import (
	"fmt"
	"github.com/yalue/elf_reader"
	"os"
)

func main() {
	// Print the section names in /bin/bash. This code will work on both 32-bit
	// and 64-bit systems.
	raw, e := os.ReadFile("/bin/bash")
	if e != nil {
		fmt.Printf("Failed reading /bin/bash: %s\n", e)
		return
	}
	elf, e := elf_reader.ParseELFFile(raw)
	if e != nil {
		fmt.Printf("Failed parsing ELF file: %s\n", e)
		return
	}
	count := elf.GetSectionCount()
	for i := uint16(0); i < count; i++ {
		if i == 0 {
			fmt.Printf("Section 0: NULL section (no name)\n")
			continue
		}
		name, e := elf.GetSectionName(uint16(i))
		if e != nil {
			fmt.Printf("Failed getting section %d name: %s\n", i, e)
			continue
		}
		fmt.Printf("Section %d name: %s\n", i, name)
	}
}
```

Reading Large Files Lazily
--------------------------

When inspecting a large ELF file offline, loading the entire image into a
`[]byte` may be undesirable. `ParseELFReader(...)` parses an ELF file from
any `io.ReaderAt` (such as an `*os.File`) plus the file's size, and reads
only the identification bytes, the ELF header, and the section/program
header tables up front:

```go
file, e := os.Open("/path/to/huge.elf")
if e != nil {
	fmt.Printf("Failed opening file: %s\n", e)
	return
}
defer file.Close()
stat, e := file.Stat()
if e != nil {
	fmt.Printf("Failed statting file: %s\n", e)
	return
}
elf, e := elf_reader.ParseELFReader(file, stat.Size())
if e != nil {
	fmt.Printf("Failed parsing ELF file: %s\n", e)
	return
}
// Use elf exactly like the result of ParseELFFile: GetSectionName,
// GetSectionContent, GetSegmentContent, GetSymbols, GetRelocations,
// DynamicEntries, and so on.
```

The returned value is the same `ELFFile` interface, backed by the same
`*ELF32File` or `*ELF64File` concrete types, and supports 32- and 64-bit
files of either endianness. Behavior notes specific to this entry point:

 - The file's `Raw` field stays `nil`; no in-memory copy of the file (or of
   a rewritten fake image) is ever built.
 - Section names, symbol tables, relocations, dynamic entries, and section
   or segment contents are read on demand, at the original file offsets
   recorded in the headers. Tables and contents located past the 4 GiB mark
   are supported, and the sparse gaps between tables and contents are never
   read.
 - Segments are returned as exactly `FileSize` bytes; the `MemorySize`
   zero-fill is not file content. NOBITS sections still return
   `UninitializedDataSectionError`. Zero-length contents at the legal end of
   the file succeed without issuing a read.
 - Every successful call returns a freshly-read copy, so modifying a
   returned slice never affects later queries, and failures are not cached.
 - Short reads and source errors are returned as errors wrapping the
   underlying cause (checkable with `errors.Is`, e.g. against `io.EOF`).
 - The caller retains ownership of the reader; the library never closes it.

