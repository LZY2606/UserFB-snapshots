# Base relocation preview

`BuildBaseRelocationPreview` (declared in
`pe-parser-library/include/pe-parse/relocation_preview.h`) answers an
offline question: if a PE image were rebased from its stored `ImageBase`
to a new one, exactly which bytes of which pointer fields would change,
and how?

It is a **pure, static byte interpreter**. It does not load an image,
execute code, modify the input buffer, or emit an executable. The input
is treated as read only; every result is copied into a self-owned report
that stays valid after the input is released.

`main.cpp` in this folder is a self-contained demonstration that builds a
minimal PE entirely in memory (no host file is opened) and prints the
returned blocks, per-operation before/after fields and merged intervals.

## Supported scope

| Image property | Supported |
| --- | --- |
| I386 PE32, little endian | yes |
| AMD64 PE32+, little endian | yes |
| `IMAGE_FILE_BYTES_REVERSED_LO` / `..._HI` set | explicitly rejected |
| Any other machine or optional-header magic | rejected |

Relocation entry types:

| Type | I386 PE32 | AMD64 PE32+ |
| --- | --- | --- |
| `ABSOLUTE` (0, sourced padding, no target read) | yes | yes |
| `HIGH` (1) | yes | rejected (machine mismatch) |
| `LOW` (2) | yes | rejected |
| `HIGHLOW` (3) | yes | rejected |
| `HIGHADJ` (4), with its paired 16-bit word | yes | rejected |
| `DIR64` (10) | rejected (machine mismatch) | yes |
| Any other type | rejected as unknown | rejected as unknown |

## Arithmetic

* The base difference is the mathematical integer
  `newImageBase - oldImageBase`; it must fit in `int64_t`, and each base
  is validated against the file's word width.
* `HIGH`/`LOW` add the high/low 16 bits of the low 32 bits of the delta,
  modulo 2^16.
* `HIGHLOW` adds modulo 2^32; `DIR64` adds modulo 2^64.
* `HIGHADJ` consumes the next whole 16-bit word in the same block as a
  signed low half (its high nibble is never treated as a type) and
  computes the new high half as
  `(((h << 16) + int16(pair) + delta + 0x8000) arith >> 16) & 0xFFFF`,
  using floor division so no negative right shift or signed overflow is
  relied on. A missing pair word is an error; a pair is never borrowed
  from another block.

## Ordering, mapping and rejection rules

* Blocks and operations are reported in raw directory order; later
  operations read the bytes previewed by earlier ones. Targets are never
  sorted, deduplicated or relocated independently from the original file.
* Overlay state is keyed by RVA. Distinct RVAs that alias one physical
  file byte therefore never cross-write, and a separate set of merged,
  RVA-sorted byte intervals is produced.
* Every byte (directory, block headers, entries and target fields) is
  located through the section table one byte at a time, so fields may
  cross adjacent sections. Gaps, overlapping ownership, virtual-only
  tails, truncation, address wrap-around and header-resident targets are
  rejected.
* A target write interval that intersects the relocation directory's own
  file bytes is unsupported.
* A missing relocation directory with a requested move yields a
  successful empty report; no values are guessed to look like pointers.
* `IMAGE_FILE_RELOCS_STRIPPED` combined with a non-zero move is rejected.
  With a zero move the directory is still fully validated.
* Every failure reports a structural category plus the offending file
  offset and/or RVA; no partial report is published, and a failed call
  leaves the stateless API ready for the next call.
