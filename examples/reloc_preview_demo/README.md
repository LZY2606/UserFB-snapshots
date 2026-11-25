# reloc_preview_demo

A small, self-contained example for
`peparse::BuildBaseRelocationPreview`. It constructs a minimal I386 PE32
image byte by byte **in memory**, then previews how the image's pointer
fields would change if `ImageBase` were moved. No host file is opened,
loaded, executed or written; the program and the library use only the C++
standard library besides pe-parse itself.

## Building against an installed pe-parse

```sh
cmake -S examples/reloc_preview_demo -B build/demo
cmake --build build/demo
./build/demo/reloc_preview_demo
```

It can also be linked directly against a freshly built static library:

```sh
c++ -std=c++11 -I pe-parser-library/include \
    examples/reloc_preview_demo/main.cpp \
    build/pe-parser-library/libpe-parse.a -o reloc_preview_demo
```

## What the preview supports

`BuildBaseRelocationPreview` is a purely static, byte-level interpreter for
the PE base relocation directory. It never loads an image, runs code,
modifies the input buffer or emits an executable.

Accepted inputs:

- Standard **little-endian** PE images only. The `IMAGE_FILE_BYTES_REVERSED_*`
  characteristics flags are explicitly rejected.
- Machine / format pairs:
  - **I386 PE32** (`IMAGE_FILE_MACHINE_I386`, optional-header magic
    `0x10B`), 32-bit `ImageBase`.
  - **AMD64 PE32+** (`IMAGE_FILE_MACHINE_AMD64`, optional-header magic
    `0x20B`), 64-bit `ImageBase`.
- Relocation types:
  - I386: `ABSOLUTE` (0), `HIGH` (1), `LOW` (2), `HIGHLOW` (3),
    `HIGHADJ` (4).
  - AMD64: `ABSOLUTE` (0), `DIR64` (10).
  - Any other type number is rejected; a type that is recognised but not
    valid for the image's machine is reported as a machine mismatch.
- The base move is the mathematical integer `new ImageBase - old ImageBase`
  and must fit in `int64_t`. Both bases are validated against the file's
  word width. A zero move still walks and validates the whole directory.
- `IMAGE_FILE_RELOCS_STRIPPED` together with a non-zero move is rejected.
- A missing relocation directory yields an empty report (the interpreter
  never guesses which values look like pointers).

Arithmetic semantics:

- `HIGH` / `LOW` add respectively the high and low 16 bits of the low 32
  bits of the delta, modulo 2^16.
- `HIGHLOW` adds the low 32 bits of the delta modulo 2^32.
- `DIR64` adds the delta modulo 2^64.
- `HIGHADJ` consumes the following 16-bit word inside the same block as its
  signed low half-word and computes
  `((h << 16) + int16(pair) + delta + 0x8000)` arithmetically shifted right
  by 16, keeping the low 16 bits. A missing pair word is an error.
- All arithmetic avoids signed overflow and implementation-defined negative
  right shifts.

Mapping semantics:

- Directory RVA, directory-internal offsets, target RVAs and file offsets
  are tracked separately; the directory may begin at a non-zero offset
  inside a section.
- Every target byte is located independently through the section table, so
  a continuous field may straddle two adjacent sections. Gaps, overlapping
  ownership, purely virtual tails, truncation and address wraparound are
  rejected. Targets inside the headers are not supported.
- Operations are replayed strictly in directory/block/entry order, so a
  later operation reads the bytes produced by earlier ones. The original
  file bytes remain frozen in the report.
- Target writes that intersect the relocation directory's file bytes are
  unsupported.
