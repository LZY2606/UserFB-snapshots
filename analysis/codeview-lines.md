# Embedded CodeView (NB09/NB11) RVA → source line analysis

The data here describes the two inert synthetic images in `analysis/samples/`
(`embedded-NB09.pe`, `embedded-NB11.pe`), produced by the
`task_support/fixtures` constructor through `cmd/codeview_samples`. Both
signatures carry byte-identical OMF tables; they are never executed or loaded
and no external PDB is consulted. Every value below is derived independently
from the constructor's raw fields and the PE section headers, not from the new
`SourceLines` output.

## 1. PE coordinates and the payload base

The debug directory points at the CodeView payload by file pointer
(`task_support/fixtures/fixtures.go`): debug data at PE file offset **0xd00**,
RVA 0x5100. Therefore every payload-relative offset `x` has a PE file offset
of `0xd00 + x` — the `*_pe` columns in `codeview-lines.tsv`. An RVA is *not* a
file coordinate and is never used as one.

The three sections (constructor `Wrap`) are:

| PE section index | name    | RVA (RelAddr) | VirtualSize | raw file offset |
|------------------|---------|---------------|-------------|-----------------|
| 1                | .first  | 0x1000        | 0x400       | 0x400           |
| 2                | .second | 0x3000        | 0x400       | 0x800           |
| 3                | .debug  | 0x5000        | 0x1000      | 0xc00           |

The optional header ImageBase is 0x140000000. It does not participate in any
mapping (see §6).

## 2. OMF directory (payload offset 0x300, header cbDirHeader=16, cbDirEntry=12, cDir=5, lfoNextDir=0)

Entries are stored shuffled; the parser keys on `iMod` rather than position.
All `lfo`/`cb` values are relative to the payload start.

| dir index | Subsection | iMod   | lfo (payload / PE file) | cb    | Meaning |
|-----------|------------|--------|--------------------------|-------|---------|
| 0 | 0x127 sstSrcModule | 7    | 0xc0 / 0xdc0  | 0xa0 | alpha source module |
| 1 | 0x120 sstModule    | 9    | 0x90 / 0xd90  | 0x30 | beta module descriptor |
| 2 | 0x12d sstSegMap    | ffff | 0x20 / 0xd20  | 0x2c | global segment map |
| 3 | 0x127 sstSrcModule | 9    | 0x180 / 0xe80 | 0xa0 | beta source module |
| 4 | 0x120 sstModule    | 7    | 0x60 / 0xd60  | 0x30 | alpha module descriptor |

Module identity is always the directory `iMod`: **7 = alpha.obj**,
**9 = beta.obj**. The directory bytes themselves live at payload 0x310…0x34c
(PE file 0x1010…0x104c).

## 3. Global segment map (sstSegMap, payload 0x20; descriptors from 0x24)

`cSeg = cSegLog = 2`; each descriptor is 20 bytes, flat 32-bit
(flags 0x0109, ovl = group = 0). Array position + 1 is the logical segment
number; `frame` is the 1-based PE section number.

| logical segment | descriptor (payload / PE) | frame (PE section) | offset | cbSeg |
|-----------------|----------------------------|--------------------|--------|-------|
| 1 | 0x24 / 0xd24 | 2 (.second) | 0x80 | 0x100 |
| 2 | 0x38 / 0xd38 | 1 (.first)  | 0x30 | 0x100 |

The mapping is deliberately reversed: logical 1 lands in physical section 2
and logical 2 in physical section 1. A logical-segment local offset `x` maps to

```
RVA = SectHdrs[frame-1].RelAddr + offset + x          (x in [0, cbSeg))
```

and the result must stay inside the section's VirtualSize. Endpoints are
checked in 64-bit arithmetic before narrowing to uint32.

## 4. Module contributions (sstModule)

| iMod | name      | sstModule (payload / PE) | contribution (logical seg, [Off, Off+cbSeg)) |
|------|-----------|---------------------------|-----------------------------------------------|
| 7 | alpha.obj | 0x60 / 0xd60 | seg 2, [0x10, 0x40) |
| 9 | beta.obj  | 0x90 / 0xd90 | seg 1, [0x40, 0x60) |

Both are `ovlNumber=0`, Style "CV". The contribution is the hard coverage
boundary: source lines that extend past it are clipped, never expanded.

## 5. Source records and line tables (sstSrcModule)

`baseSrcFile` and `baseSrcLn` are relative to the enclosing sstSrcModule
start, so the records can be — and here are — non-contiguous with the headers.

### alpha — sstSrcModule payload 0xc0 (PE 0xdc0), iMod 7, logical seg 2

- header segment range (closed): [0x12, 0x2a], seg = 2
- source file record via baseSrcFile 0x30 → payload **0xf0** (PE **0xdf0**):
  `alpha.c`, file segment range (closed) [0x12, 0x2a]
- line table via baseSrcLn 0x70 → payload **0x130** (PE **0xe30**), seg 2,
  cLnOff 2:

| line | offset | element offset (payload / PE) |
|------|--------|--------------------------------|
| 7  | 0x14 | **0x134** / **0xe34** |
| 9  | 0x20 | **0x138** / **0xe38** |

### beta — sstSrcModule payload 0x180 (PE 0xe80), iMod 9, logical seg 1

- header segment range (closed): [0x40, 0x5f], seg = 1
- source file record via baseSrcFile 0x30 → payload **0x1b0** (PE **0xeb0**):
  `beta.c`, file segment range (closed) [0x40, 0x5f]
- line table via baseSrcLn 0x70 → payload **0x1f0** (PE **0xef0**), seg 1,
  cLnOff 2:

| line | offset | element offset (payload / PE) |
|------|--------|--------------------------------|
| 41 | 0x40 | **0x1f4** / **0xef4** |
| 44 | 0x50 | **0x1f8** / **0xef8** |

## 6. Two worked mapping chains

### alpha.obj line 7, query RVA 0x1044

1. Directory entry 0 (iMod 7, sstSrcModule 0xc0) and entry 4 (iMod 7,
   sstModule 0x60) establish the module. Its contribution is logical seg 2,
   local [0x10, 0x40).
2. The segmap entry for logical segment 2 (payload 0x38) gives frame 1,
   offset 0x30 → base RVA = `.first`.RelAddr 0x1000 + 0x30 = **0x1030**.
3. Line 7's table offset 0x14 lies within the module range [0x12, 0x2a],
   file range [0x12, 0x2a] and contribution [0x10, 0x40). It covers
   [0x14, 0x20) because the next strictly greater offset is 0x20.
4. RVA interval = 0x1030 + [0x14, 0x20) = **[0x1044, 0x1050)**. Query 0x1044
   is inside; the half-open end 0x1050 does not hit line 7 (it hits line 9).
   Byte trail: subsection 0xc0, file record 0xf0, line element 0x134
   (PE file 0xdc0, 0xdf0, 0xe34).

### beta.obj line 41, query RVA 0x30c0

1. Directory entry 3 (iMod 9, sstSrcModule 0x180) and entry 1 (iMod 9,
   sstModule 0x90): contribution logical seg 1, local [0x40, 0x60).
2. Segmap logical segment 1 (payload 0x24) gives frame 2, offset 0x80 →
   base RVA = `.second`.RelAddr 0x3000 + 0x80 = **0x3080**.
3. Line 41's offset 0x40 covers [0x40, 0x50) (next strictly greater offset
   0x50), inside module/file range [0x40, 0x5f] and contribution
   [0x40, 0x60).
4. RVA interval = 0x3080 + [0x40, 0x50) = **[0x30c0, 0x30d0)**. Byte trail:
   subsection 0x180, file record 0x1b0, line element 0x1f4
   (PE file 0xe80, 0xeb0, 0xef4).

### Uncovered query 0x1060 (and the right-edge rule at 0x105b)

The last alpha line (line 9) runs from offset 0x20 to the closed file
segment end plus one, i.e. [0x20, 0x2b) → RVA **[0x1050, 0x105b)**. The
contribution ends at local 0x40 (RVA 0x1070), but the source records end at
0x2b, so nothing legitimately describes [0x105b, 0x1070):

- **0x105b** is the exclusive right edge of line 9 → `UNCOVERED`. The line
  table has no entry for that address, and the half-open interval end never
  counts as a hit.
- **0x1060** is beyond the file/module segment end altogether →
  `UNCOVERED`. The contribution hole must not be filled by stretching the
  preceding line forward.

Likewise beta's last line 44 covers [0x30d0, 0x30e0); query 0x30e0 returns
`UNCOVERED`.

## 7. Why logical segments are not PE section numbers

The logical segment number is the array position in sstSegMap and is only an
index into that table. The physical destination is an independent descriptor
field, `frame`, plus a per-descriptor `offset`. In these samples the ordering
is reversed on purpose: logical segment 1 maps to PE section 2 (frame 2) at
+0x80, while logical segment 2 maps to PE section 1 (frame 1) at +0x30.
Treating the logical number as a 1-based section index would send beta's
lines to `.first` instead of `.second` and misplace every RVA by 0x2000 or
more. All translation therefore goes through the global segment map first,
then intersects the module contribution, source-module segment and
source-file segment ranges.

## 8. Why ImageBase is never added

Queries are RVAs — addresses relative to the image base. The mapping formula
in §3 adds only a section `RelAddr` (itself an RVA), the segmap `offset` and
the line-table offset. The optional-header ImageBase (0x140000000) would only
be added to obtain a load-time virtual address (VA). Adding it here would
turn 0x1044 into 0x140001044, which matches no section and no record. The
custom regression images use a different ImageBase (0x180000000) precisely to
prove identical RVA results regardless of that field.

## 9. Reproducing the artifacts

```sh
# regenerate the two samples from the fixture constructor
go run ./cmd/codeview_samples
# regenerate the TSV through the production ParseFile/SourceLines entry point
go run ./examples/codeview_lines analysis/samples/embedded-NB09.pe \
    0x1044 0x1050 0x105a 0x105b 0x1060 0x30c0 0x30d0 0x30df 0x30e0 \
    > analysis/codeview-lines.tsv   # plus the NB11 rows in the committed file
```

The TSV carries each record's offsets twice: payload-relative
(`subsection_offset`, `file_offset`, `line_offset`) and as PE file offsets
(`subsection_pe`, `file_pe`, `line_pe` = payload offset + 0xd00), so every
row can be checked against the original bytes without interpreting any RVA as
a file position.
