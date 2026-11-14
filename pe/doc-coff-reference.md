# COFF reference inspection and BigObj v2 support

This document describes the index spaces used by `File.COFFReference` and
the BigObj v2 reading support added alongside it. Everything here is a
static interpretation of bytes on disk: object code is never executed,
relocations are never applied, and no linker-side symbol selection is
performed. Weak-external chains report the fallback relationship the
object *declares* for an absent symbol, not the outcome of a link.

## Supported inputs

| Input | Header | Symbol stride | Read | Write |
|---|---|---|---|---|
| Ordinary COFF object | 20-byte `FileHeader` | 18 bytes | yes | yes (unchanged) |
| PE32 image | DOS/PE stub + 20-byte `FileHeader` + `OptionalHeader32` | 18 bytes | yes | yes |
| PE32+ image | DOS/PE stub + 20-byte `FileHeader` + `OptionalHeader64` | 18 bytes | yes | yes |
| BigObj v2, AMD64 (`IMAGE_FILE_MACHINE_AMD64`) | 56-byte BigObj header | 20 bytes | yes | **not supported** |

BigObj detection requires more than the shared `00 00 ff ff` opening
signature: a BigObj must also carry version `2` at header offset 4. A
[short import object](https://learn.microsoft.com/en-us/windows/win32/debug/pe-format#import-object-header-format)
opens with the same signature but a different header layout and version,
so `NewFile` rejects it with an explicit error rather than misreading it
as a BigObj. Other BigObj machine types and versions are also rejected.

`File.Bytes` and `File.WriteFile` return an explicit "not supported" error
for BigObj files. Ordinary COFF and PE files continue to serialize symbol
records as 18 bytes, including when an in-memory `COFFSymbol.SectionNumber`
holds a value that only fits 32 bits because of BigObj widening.

## On-disk layout that drives the reader

BigObj v2 uses a 56-byte header followed immediately by ordinary 40-byte
COFF section headers; there is no DOS or PE optional header. Relevant
header fields:

| Offset | Size | Meaning |
|---|---|---|
| 0 | 2 | Signature word 1 (`0x0000`) |
| 2 | 2 | Signature word 2 (`0xffff`) |
| 4 | 2 | Version (`2`) |
| 6 | 2 | Machine (`0x8664` for AMD64) |
| 8 | 4 | TimeDateStamp |
| 12 | 16 | ANLONI ClassID |
| 28 | 16 | SizeOfData, Flags, MetaDataSize, MetaDataOffset |
| 44 | 4 | NumberOfSections (32-bit) |
| 48 | 4 | PointerToSymbolTable |
| 52 | 4 | NumberOfSymbols, auxiliary slots included |

The full 56-byte header is exposed on `File.BigObjHeader`. The embedded
`File.FileHeader` still represents a standard 20-byte COFF header shape:
its `Machine`, `TimeDateStamp`, `PointerToSymbolTable` and
`NumberOfSymbols` mirror the BigObj fields, while `NumberOfSections`
stays zero because the real count cannot fit its 16-bit field. Use
`File.SectionCount() uint32` for the authoritative section count on every
file kind; `File.IsBigObj()` distinguishes the two header families.

A BigObj symbol slot is 20 bytes. Relative to the ordinary 18-byte record
only `SectionNumber` changes, from a signed 16-bit field at offset 12 to a
signed 32-bit field. Auxiliary slots share the owner's stride and remain
opaque unless the owning primary symbol defines their format (the weak
external case below). The string table begins at
`PointerToSymbolTable + NumberOfSymbols * stride`, so its position differs
between 18-byte and 20-byte objects; long section and symbol names resolve
through it exactly as before.

## Widened fields and special section numbers

`COFFSymbol.SectionNumber` and `Symbol.SectionNumber` are signed 32-bit
values. This preserves:

- defining section numbers greater than 65535 without truncation;
- `0` — undefined symbol (a non-zero `Value` then states a common size);
- `-1` — absolute symbol;
- `-2` — debug symbol.

The special values are never treated as section indexes. Positive values
remain one-based section numbers.

## Index spaces used by COFFReference

`func (f *File) COFFReference(sectionIndex, relocationIndex uint32) (*COFFReference, error)`

- **Section index**: one-based, matching COFF section numbers. Valid range
  is `1..File.SectionCount()`. Zero and out-of-range values are errors.
- **Relocation index**: zero-based position inside the named section's
  `Relocs` slice. Each on-disk relocation is 10 bytes
  (`VirtualAddress uint32`, `SymbolTableIndex uint32`, `Type uint16`) and
  its file position is reported as `RelocationOffset`.
- **Symbol slot index**: relocation `SymbolTableIndex` values and weak
  auxiliary `TagIndex` values address the *complete original symbol
  table*, including auxiliary records. This is the same index space as
  `File.COFFSymbols`. The auxiliary-free `File.Symbols` slice is never
  indexed by this API, because doing so would misaddress every table that
  contains auxiliary records.
- **Slot identity**: two primary symbols with the same name remain two
  distinct slots and are never merged. Every chain hop carries its raw
  slot index and file position (`PointerToSymbolTable + slot*stride`).

A returned `COFFReference` owns all of its slices and strings. Mutating a
result never changes `File` state, and reads and queries never modify the
underlying input bytes. A failed query returns an error (a
`*COFFReferenceError`, carrying the offending raw slot index and file
position where applicable) and never a half-built chain; later queries on
the same file remain valid.

## Weak external fallback chains

A canonical weak external has storage class `105`
(`IMAGE_SYM_CLASS_WEAK_EXTERNAL`), section number zero and exactly one
auxiliary slot. The first eight bytes of that auxiliary record are
`TagIndex uint32` followed by `Characteristics uint32`; the auxiliary
slot's own index and file position are reported alongside them. Supported
characteristics, preserved with their raw numeric value on every hop:

| Value | Constant | Meaning |
|---|---|---|
| 1 | `IMAGE_WEAK_EXTERN_SEARCH_NOLIBRARY` | no library search |
| 2 | `IMAGE_WEAK_EXTERN_SEARCH_LIBRARY` | library search allowed |
| 3 | `IMAGE_WEAK_EXTERN_SEARCH_ALIAS` | alias of the tagged symbol |

Any other characteristic, a missing auxiliary record, a `TagIndex`
pointing outside the table or at an auxiliary slot, and a chain that loops
back to a visited slot are all errors. The same intermediate or terminal
slot may be shared by several references; sharing does not merge slots.

## Terminal classification

The final primary slot of every chain is classified statically:

- `COFFTerminalDefined`: positive section number; reports the section name,
  the symbol's in-section offset (`Value`), and a raw-byte file position
  only when `Offset + Value` names an actual byte of that section's raw
  data. Zero-fill sections and offsets beyond `SizeOfRawData` report
  `HasDataOffset == false` instead of an invented location. No load
  address or relocation arithmetic is computed.
- `COFFTerminalUndefined`: section number `0`, value `0`.
- `COFFTerminalCommon`: section number `0`, non-zero value giving the
  declared common size.
- `COFFTerminalAbsolute`: section number `-1`.
- `COFFTerminalDebug`: section number `-2`.

## Compatibility notes

- Existing entry points (`NewFile`, `Open`, `Section.Data`, section
  `Relocs`, long section and symbol names, the string table) keep their
  signatures and behavior for ordinary COFF, PE32 and PE32+ files.
- No previously exported identifier was removed. The only field-type
  changes are the widening of `COFFSymbol.SectionNumber` and
  `Symbol.SectionNumber` from `int16` to `int32`.
- Ordinary PE files remain writable with 18-byte symbol records; BigObj
  files are read-only in this package.
