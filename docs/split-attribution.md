# Split-DWARF function attribution

The `elf` package can recover function-level attribution from a stripped,
linked ELF image plus its DWARF v5 package (`.dwp`) without executing the
image, opening the source paths embedded in the debug info, or looking for
loose `.dwo` files. All inputs are read as bytes; the package and the
function results own the bytes they need, so closing the ELF files or
overwriting the original input buffers cannot change a later recovery.

## When this path applies

The supported envelope is deliberately narrow:

- uncompressed, linked, little-endian **ELF64**;
- **DWARF32, version 5** only;
- image units of type `DW_UT_skeleton` (0x04), package units of type
  `DW_UT_split_compile` (0x05);
- a version 5 64-bit `.debug_cu_index` (no TU index, no GNU version 2
  index);
- no relocations and no external supplementary objects.

Function ranges are recovered from exactly the forms emitted by the shipped
samples:

- `DW_FORM_addrx*` on `DW_AT_low_pc` together with a constant
  `DW_AT_high_pc`, and
- `DW_FORM_rnglistx` on `DW_AT_ranges`, where the referenced list contains
  `DW_RLE_offset_pair` (0x04) entries terminated by `DW_RLE_end_of_list`
  (0x00). `offset_pair` operands are interpreted relative to the paired
  skeleton's own `DW_AT_low_pc`.

Anything outside this envelope (64-bit DWARF, other unit types, other range
opcodes such as `base_addressx`, compressed sections, relocations, a TU
index, a table reference that leaves its owning contribution, …) returns a
`*SplitError` naming the object, section and offset. A relative value is
never turned into a final address by guessing.

## API

### Package index: `File.DWARFPackage`

```go
pkg, err := dwpFile.DWARFPackage() // *DWARFPackage, reusable across images
```

`DWARFPackage` parses `.debug_cu_index` once and keeps private copies of
every package section it references, including the package-wide shared
`.debug_str.dwo`. Repeated calls on the same `File` return the same package.

`Lookup(id uint64) DWPIndexResult` implements the version 5 double-hashing
open-addressing probe over 64-bit signatures. A DWO identifier is itself a
64-bit hash, so both hash functions use its halves: the home bucket is
`id mod bucket_count` and the odd probe stride is
`((id >> 32) mod (bucket_count - 1)) | 1`, which is always coprime with the
power-of-two bucket count. The result distinguishes a hit from a
terminating miss:

- `result.Found()` / `result.Unit` on success;
- `result.Miss` otherwise, with `DWPMissEmptySlot` (an empty bucket proves
  the id is absent) or `DWPMissTableFull` (every bucket was probed), plus
  the terminating bucket and probe count.

A `DWPUnit` carries the raw 64-bit `Signature`, the **1-based** `IndexRow`,
and one `DWPContribution` per section identifier. Each contribution records
its section identity, package-relative offset, length and absolute package
file offset. Bucket numbers, matrix column positions and file ordering are
never used as unit identities. Column identifiers the parser does not
understand are retained in `UnknownColumns` with their id and byte range;
they are never silently dropped.

### Function recovery: `File.SplitFunctions`

```go
functions, err := image.SplitFunctions(pkg) // []SplitFunction
```

Skeleton units in the image and split units in the package are paired
**only** by the 64-bit DWO identifier found in both unit headers. The split
header's identifier is checked against the skeleton's identifier before any
DIE is read. CU order, `dwo_name`, function names and source file names
take no part in pairing.

Each skeleton supplies its own `DW_AT_addr_base` and `DW_AT_low_pc`; an
`addrx` index can only read entries inside that skeleton's `.debug_addr`
contribution, and an `offset_pair` is only ever based on that skeleton's
low PC. Reusing one package for two images never leaks one image's address
base into the other.

Function identity is the DWO identifier plus the function DIE's original
byte position, so same-named local functions in different units stay
separate records. A function described by several ranges keeps one record
with several `SplitRange` entries. Declarations without code ranges are
ignored.

### PC selection: `SelectSplitFunctions`

```go
matches := elf.SelectSplitFunctions(functions, pc)
```

Ranges are half-open `[Low, High)`: a PC equal to a low bound matches and a
PC equal to the high bound does not. A miss returns an empty, non-nil slice;
no nearest function is guessed.

## Coordinate units

Every byte position is a `SplitPosition` stating both which object owns the
bytes (`"image"` or `"dwp"`) and two offsets:

- `SectionOffset`: relative to the start of the named section;
- `FileOffset`: absolute offset inside the ELF/DWP file.

Function DIE positions are given three ways: relative to the unit's
`.debug_info.dwo` contribution (`FunctionDIELocal`), relative to the whole
`.debug_info.dwo` section (`FunctionDIESection`), and as an absolute package
offset (`FunctionDIEFile`). These always point at the original bytes; the
standard library's `debug/dwarf` is used only to decode DIEs through owned,
re-based section views, and its internal buffer positions are never reported
as evidence.

When an `addrx` form is used, `SplitFunction.Addr` records the raw index,
the skeleton `addr_base`, the exact `.debug_addr` entry position and the
value read. When an `rnglistx` form is used, `SplitFunction.Rnglist`
records the raw index, the offset-table entry position, the first opcode
byte of the referenced list and the skeleton low PC used as the range base.

## Worked example: two same-named `transform` records

Both images were built from the same compilation inputs, so the local
function `transform` exists twice in each image — once in the `ingress.c`
unit and once in the `egress.c` unit. Looking at the `ingress.c` copy
across the two images shows how identity, address context and contributions
support the attribution.

| Fact | primary | alternate |
| --- | --- | --- |
| DWO id | `0x45f708c196b27967` | `0x45f708c196b27967` |
| CU index row | 1 | 3 |
| Skeleton header (section / file) | `0x00` / `0x1113` | `0x50` / `0x115f` |
| Function DIE, local / section offset | `0x40` / `0x40` | `0x40` / `0xf8` |
| Skeleton low PC (rnglist base) | `0x401000` | `0x503080` |
| Resolved range | `[0x401030, 0x40104a)` | `[0x5030b0, 0x5030ca)` |
| Rnglist offset entry (file offset) | `0x198` | `0x44b` |
| Rnglist list first byte (file offset) | `0x19c` | `0x44f` |

(The exact bytes are in `reports/primary.json` and
`reports/alternate.json`, produced by the example command.)

1. **DWO identity.** The identical 64-bit id ties the skeleton in each
   image to the same logical compilation unit even though the package was
   packed in a different order: the unit is row 1 in one package and row 3
   in the other. The row number is recorded as evidence, but pairing never
   relies on it — lookup follows the hash probe, and the split header's id
   is re-checked against the skeleton.
2. **Address context.** Both `transform` DIEs carry `rnglistx` index 0 and
   the same relative `offset_pair` (0x30, 0x4a). The numbers only become
   link-time addresses through each image's own skeleton low PC, producing
   `0x401030..0x40104a` in primary and `0x5030b0..0x5030ca` in alternate.
   Nothing is copied from one image's analysis into the other.
3. **Contributions.** The rnglist bytes are located through the unit's own
   RNGLISTS contribution in each package (different package offsets), not
   through section order or a neighboring unit's table. The INFO, ABBREV,
   STR_OFFSETS and RNGLISTS contribution ranges attached to each record
   bound every table access; a reference crossing those bounds is an error
   rather than a borrowed answer.

## Example command

`examples/split-attribution` is a thin command-line wrapper; it contains no
recovery logic and no sample-specific values.

```
split-attribution -image device.elf -dwp device.elf.dwp -queries queries.json
```

It prints one JSON document to stdout:

- `functions`: the complete array of `SplitFunction` records;
- `queries`: one entry per input PC, in input order, with the numeric `pc`
  and a `matches` array that is present (and empty on a miss) for every
  query.

## Limitations

The chain deliberately stops at the supported envelope listed above. It
does not recover function-name string byte spans, type layouts, line
programs, inlined-subroutine chains or variable expressions, and it never
fills an unknown address with zero. A structural problem — a skeleton with
no package unit, a duplicate DWO identity, a missing contribution, an
out-of-bounds table reference, an unsupported opcode — fails the whole call
with a `*SplitError` instead of returning a partial, plausible-looking
function list.
