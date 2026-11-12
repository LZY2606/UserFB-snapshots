# SAV import-chain regression tests

This repository's SAV import chain (dictionary → segmented long strings →
row decompression → projected/windowed delivery) is covered by four C test
groups in `sanity/target.c`, with helpers in `sanity/savkit.h`. They are
registered through `sanity/cases.h` via `task_register_tests()` and run with:

```sh
bash sanity/run.sh target
```

`run.sh` compiles `src/*.c` and `src/spss/*.c` into a static library, builds
`sanity/main.c` + `sanity/target.c` against it, runs the four groups (each
returns 0 only when every assertion passes), then re-runs the existing
`baseline` import check. Every group materializes its `.sav` inputs in a
fresh temporary directory (`$TMPDIR`, falling back to `/tmp`) and removes
them afterwards; no downloaded samples are used. Production code and the
existing baseline assertions are untouched.

## Why an independent oracle

If test material is produced by this library's own writer and then read
back by its reader, a shared misunderstanding of the format cancels out.
The inputs below therefore come from two sources:

* **Independent material** (`sanity/savkit.h`): dictionary and data area
  are emitted byte by byte from the format description, with explicit
  little-endian putters. It does not call the ReadStat writer, and the
  reader's and compressor's loops are not copied into it. `SegmentLayout`
  and `CompressedRows` run entirely on such material; the compressed
  reference stream in `CompressedRows` is hand-assembled byte by byte.
* **Comparison material**: the ReadStat writer (`readstat_begin_writing_sav`
  etc.), used in `IndependentOracle` to show both encodings of the same
  logical model parse to the same values.

The reference values are never produced by the library under test:

* expected strings/numbers come from the logical row model in
  `sanity/target.c` (deterministic position-dependent pattern bytes,
  explicit UTF-8 characters, explicit system-missing flags);
* expected compressed rows come from `ok_expand()` in `sanity/savkit.h`,
  an independent expansion of the hand-built opcode stream into 8-byte
  units, checked both against `sav_decompress_row()` (unit bytes) and
  against `readstat_parse_sav()` (logical row values).

## Independent material: field layout

All integers little-endian; IEEE-754 doubles; bias 100.0.

**File header (176 bytes)** — `"$FL2"`; 60-byte space-padded product name;
`layout_code=2`; `nominal_case_size` = number of 8-byte columns per row;
`compression` (0 none / 1 rows); `weight_index=0`; `ncases`; `bias=100.0`;
`creation_date` (9 bytes); `creation_time` (8 bytes); 64-byte file label;
3 padding bytes.

**Variable records** — `rec_type=2`, then `type` (0 numeric, >0 string
width, -1 continuation), `has_var_label=0`, `n_missing_values=0`, print and
write format codes (`F8.2` for numerics, `A<w>` for strings), 8-byte
space-padded upper-case short name. A string of storage width W occupies
ceil(W/8) columns: one base record plus ceil(W/8)-1 continuation records
(`type=-1`).

**Long-string segmentation** — a declared width W > 255 is stored in
`ceil((W+4)/252)` segments. Every full segment holds 255 data bytes in 256
bytes of storage; the 256th byte is padding and must not enter the
delivered string. The last segment stores the remaining `W - 252*(nseg-1)`
bytes rounded up to a multiple of 8. The dictionary carries one base
record (`type=255`) and one ghost record per further segment (named after
the first five characters of the logical name plus a counter), each with
its own continuations. Info record subtype 14 (`SHORT=00511\0\t` tuples)
raises the logical width; subtype 13 (`SHORT=longname` tuples, tab
separated) carries the logical variable names.

**Other dictionary records** — subtype 3 (machine integer info:
little-endian, IEEE, character code 65001), subtype 4 (SYSMIS =
`0xFFEFFFFFFFFFFFFF`, HIGHEST = `0x7FEFFFFFFFFFFFFF`, LOWEST =
`0xFFEFFFFFFFFFFFFE`), subtype 20 (`"UTF-8"`), subtype 16 (extended number
of cases), then `rec_type=999` + filler to terminate the dictionary.

**Data area, uncompressed** — per row, per variable: one 8-byte double
(SYSMIS bit pattern for system-missing) or the string bytes: content,
space-padded to the declared width, distributed 255 bytes per 256-byte
segment, segment padding bytes included in storage.

**Data area, row-compressed** — control bytes in groups of 8, literal
8-byte units interleaved after each group. Opcodes: `0` = padding (no
output); `1..251` = double `code - 100`; `252` = end of data; `253` =
next 8 bytes are a literal unit; `254` = eight spaces; `255` = SYSMIS.
The reference implementation of these semantics is `ok_expand()`.

## The four groups

**SegmentLayout** — widths 255, 256, 257, 510, 511 (both sides of the
255/256 single-segment cut and the 510 = 2×255 two-segment cut), with
numerics before and after. Content is a position-dependent non-repeating
pattern; a 3-byte UTF-8 character straddles the 255/256 content cut, a
2-byte one the 510/511 cut; one cell is blank (all spaces); one cell's
content ends in spaces ahead of the padding. Assertions land in the
callbacks on: logical variable count, variable names, storage widths, and
the exact reassembled string bytes (the reader trims trailing spaces, so
expectations are the content right-trimmed), plus numeric values.

**CompressedRows** — one hand-built 56-byte stream: integer opcodes
(105/97/142/107/100), 253 literals (`"AB"`, `2.5`, `"XYZ"`, `100.5`,
`"Q"`), 254 spaces, 255 SYSMIS, a zero opcode mid-stream, and 252
termination; the first control group ends one unit into logical row 2, so
control groups straddle row boundaries. Checks: `sav_decompress_row()`
reproduces the independently expanded units row by row; a full SAV
carrying the stream parses to the expected logical values through
`readstat_parse_sav()`; the uncompressed rendering of the same units
parses to the identical delivery; a file truncated in the middle of a
literal (36 bytes, cutting `"XYZ"`) fails with `READSTAT_ERROR_PARSE`,
and the truncated stream leaves `sav_decompress_row()` at
`SAV_ROW_STREAM_NEED_DATA` after two complete rows.

**ProjectionWindow** — numeric / 300-byte string / numeric / numeric,
row-compressed. Skipping the numeric column before, between, and after
the long string (and all three at once) leaves the remaining values
unshifted, consumed at physical column widths. `row_offset`/`row_limit`
only change the delivered row set; delivered row numbers restart at 0.
Original `index` and `index_after_skipping` are checked per variable.
Aborting from the value callback yields `READSTAT_ERROR_USER_ABORT` with
the delivered prefix intact, and a fresh parser reads the same file
completely. Edge cases asserted: zero-row file, all columns skipped,
window past the end of the data.

**IndependentOracle** — one logical model (numerics incl. a SYSMIS cell,
an 8-byte string, a 300-byte string) materialized four ways: independent
oracle uncompressed, oracle compressed, ReadStat writer uncompressed,
writer compressed. All four parses must deliver the model exactly
(values, names, widths, row counts, compression flags).

## Mutation checks (all reverted afterwards)

Three temporary degradations were applied to prove the content assertions
catch them. Each still compiled and ran; none was "caught" by a build
failure, an unparsed file, or a bare non-zero exit — each tripped the
specific assertions below. `RUN_EXIT` is the exit status of
`bash sanity/run.sh target` (0 = pass, 1 = test failure).

| # | Modification point | Change | Assertions hit | Exit |
|---|--------------------|--------|----------------|------|
| M1 | `src/spss/readstat_sav_read.c`, `sav_process_row`: `read_len = 8 - (offset == 31)` | read all 8 bytes of the last column, i.e. keep the 256th storage byte of each long-string segment | `SegmentLayout`: `readstat_parse_sav rc=17`; `ProjectionWindow`: `s1 row 0 mismatch at byte 256`; `IndependentOracle`: `row 0 longtext mismatch at byte 256`; baseline: `DIFF string-segment` | 1 |
| M2 | `src/spss/readstat_sav_compress.c`, `sav_decompress_row`: `fp_value = chunk[i] - bias` | integer control codes decoded with bias off by one (`+ 1`) | `CompressedRows`: `memcmp(rowbuf, units...)` (decompressed units vs independent expansion); `ProjectionWindow`: `n2 row 0 = 21.000000, want 20.000000`; `IndependentOracle`: `row 0 num2=1.000000`; baseline: `DIFF numeric-token` | 1 |
| M3 | `src/spss/readstat_sav_read.c`, `sav_process_row` double branch: `if (!ctx->variables[...]->skip)` | skipped numeric columns delivered to the value callback anyway (`if (1)`) | `ProjectionWindow`: `cell 0 row=0 var=0, want row=0 var=1`; baseline: `DIFF skipped-column` | 1 |

After each run the production file was restored with `git checkout`; the
final state passes all four groups plus the baseline (`RUN_EXIT=0`).
