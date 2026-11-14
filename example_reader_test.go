package elf_reader_test

import (
	"fmt"
	"os"

	elf_reader "github.com/yalue/elf_reader"
)

// Demonstrates how to inspect an ELF file lazily through an io.ReaderAt,
// without reading the entire file into memory first. Only the ELF header and
// the section/program header tables are read during parsing; section names,
// symbols, relocations, and section/segment contents are read from the
// source on demand, at their original file offsets. The caller keeps
// ownership of the reader (and closes it, if applicable); the library never
// does.
func ExampleParseELFReader() {
	file, e := os.Open("test_data/sleep_amd64")
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
	fmt.Printf("Machine: %s\n", elf.GetMachineType())
	fmt.Printf("Sections: %d\n", elf.GetSectionCount())
	name, e := elf.GetSectionName(1)
	if e != nil {
		fmt.Printf("Failed getting section 1 name: %s\n", e)
		return
	}
	fmt.Printf("Section 1: %s\n", name)
	// Output:
	// Machine: AMD64
	// Sections: 29
	// Section 1: .interp
}
