# COFF relocation references and BigObj support

This package reads ordinary COFF objects, PE32/PE32+ images, and AMD64
BigObj v2 (extended COFF) objects from disk. Everything here is a static
description of file contents: nothing is loaded, executed, relocated, or
linked.

## BigObj detection and limits

`pe.NewFile` recognizes a BigObj by the full 56-byte v2 header: the
`00 00 ff ff` signature, `Version == 2`, `Machine == 0x8664` (AMD64), and
the BigObj ClassID (`c7 a1 ba d1 ee ba a9 4b af 20 fa f6 6a a4 dc b8`).
The signature alone is not enough: short import objects share it and are
rejected with an explicit error, as are BigObj headers for other machines.

For a BigObj file:

- `File.BigObjHeader` is non-nil and holds the raw 56-byte header;
  `File.IsBigObj()` reports true.
- `File.SectionCount() uint32` returns the real 32-bit section count,
  which may exceed 65535. `FileHeader.NumberOfSections` stays a `uint16`
  and keeps describing the ordinary disk header of regular files.
- `FileHeader` carries the matching BigObj values where field widths
  allow (machine, timestamp, symbol table pointer, symbol count).
- Symbol records are 20 bytes (`SectionNumber` is a signed 32-bit field);
  the string table follows the symbol table at the matching stride, so
  long section names and long symbol names resolve through the existing
  `Section.Name` / `Symbol.Name` / `COFFSymbol.FullName` entries, and
  `Section.Data` and `Section.Relocs` work as before.
- `COFFSymbol.SectionNumber` and `Symbol.SectionNumber` are `int32`, so
  definition sections above 65535 are not truncated and 0 (undefined or
  common), -1 (absolute) and -2 (debug) keep their COFF meaning.

Ordinary COFF objects and PE32/PE32+ images are unchanged: 18-byte symbol
records, existing headers, and existing read/write behavior. Writable
files are still written with 18-byte symbol records. BigObj files are
read-only for now: `Bytes` and `WriteFile` return an explicit
"not supported" error for them.

## Reference queries

`File.COFFReference(sectionIndex, relocationIndex uint32)` resolves one
COFF relocation to the symbol it refers to and returns a
`*COFFReference` that owns its data (it shares no mutable slices with the
`File`, and neither reading nor querying mutates the input).

Index spaces:

- `sectionIndex` is 1-based over `File.SectionCount()` /
  `File.Sections`.
- `relocationIndex` is 0-based over `Section.Relocs` of that section.
- `Reloc.SymbolTableIndex` and weak-external `TagIndex` address the raw
  symbol slot table (`File.COFFSymbols`), auxiliary records included.
  The aux-stripped `File.Symbols` list is never indexed, and slots are
  never merged by name: two symbols with the same name keep distinct
  slot identities.

The result records the source section, the relocation index and the file
offset of the relocation record, and the original relocation type and
offset. Each hop of `Chain` lists the raw slot index, the slot's file
offset, name, value, storage class, and full signed section number. A hop
through a weak external (storage class 105) also interprets the auxiliary
record that immediately follows it: `TagIndex`, `Characteristics`, and the
auxiliary record's slot index and file offset, and the chain continues at
`TagIndex`. Search characteristics 1 (`SEARCH_NOLIBRARY`), 2
(`SEARCH_LIBRARY`), and 3 (`SEARCH_ALIAS`) are preserved per hop; other
values are an error.

The chain is the fallback path the object *declares* for the case that a
symbol stays undefined. It is not a link result: no library search is
performed and no symbol definition is selected.

The terminal symbol is classified by its section number:

- positive: `COFFTerminalDefined`, with the 1-based section index, the
  offset of the symbol value within the section, and — only when the
  value names an actual raw byte of that section — the matching file
  offset (`HasFileOffset`). Zero-fill and out-of-raw-range values are
  reported explicitly without a file offset. No load address is computed.
- 0 with value 0: `COFFTerminalUndefined`.
- 0 with value > 0: `COFFTerminalCommon`.
- -1: `COFFTerminalAbsolute`.
- -2: `COFFTerminalDebug`.

Failures return a `*COFFReferenceError` and no partial chain. Errors
caused by a symbol slot — out-of-range slot index, an index pointing at
an auxiliary slot, a weak external without an auxiliary record, unknown
search characteristics, or a fallback cycle — carry the raw slot index
and its file offset (`HasSlot`, `SlotIndex`, `SlotOffset`). Out-of-range
section or relocation indexes fail without slot information. A failed
query does not affect later queries, and the same intermediate symbol may
be shared by any number of references.
