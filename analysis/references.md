# Compressed ELF symbol reference analysis

This directory is generated, not hand-authored data. `references.tsv` is
produced by `task_support/analysis.c`, which builds two in-memory ELF
objects through `task_fixture()` (one ordinary, one with independently
compressed sections 2..5), writes them to this directory, and reads them
back through the public `elf_getdata` / `elf_strptr` / GElf interfaces.
No value in the table is hard-coded or read from a stored answer; every
column comes from a library call on the generated input.

## How to reproduce (fully offline)

```sh
# 1. Build libelf (including the bundled zlib sources), the baseline and
#    the analysis program.  No system zlib is used.
make -f task_support/Makefile -j4
make -f task_support/Makefile .task-build/analysis

# 2. Generate plain.elf and compressed.elf and the reference table.
mkdir -p analysis
.task-build/analysis analysis

# 3. Run the regression contract (views, symbols, failures) plus the
#    normal-read baseline.
bash task_support/run.sh target
```

## The single reference row

Both inputs produce exactly one relocation record, and every
symbol-reference column is identical between them:

| column | value | meaning / source |
|---|---|---|
| `rela_scn` | 5 | relocation section index; its `sh_info=1`, `sh_link=2` |
| `rela_record` | 0 | record number within section 5 |
| `target_scn` | 1 | from the relocation section's `sh_info`: the section holding the referenced location |
| `r_offset` | `0x8` | referenced location: section 1, logical offset 8 |
| `r_type` | `2` | relocation type, kept as its numeric value (no machine-specific name is asserted) |
| `r_addend` | `-4` | signed addend, preserved verbatim |
| `symtab_scn` | 2 | from the relocation section's `sh_link` |
| `sym_index` | 1 | symbol table 2, entry 1 |
| `sym_name` | `target` | symbol 1's `st_name=192` resolved through section 2's `sh_link` (string table 3) |
| `raw_st_shndx` | `65535` (`SHN_XINDEX`) | the raw on-disk `st_shndx` of symbol 1 |
| `def_scn` | 1 | expanded definition section, taken from the `SHT_SYMTAB_SHNDX` companion |
| `st_value` | `0x40` | definition offset inside its section |
| `st_size` | `8` | size of the defined symbol |

### Reference position vs. definition position

- The **reference** is a relocation, not an instruction address.
  Section 5 record 0 says: at section 1 logical offset `0x8` there is a
  relocation against symbol 1 of type 2 with addend `-4`. Nothing is
  applied or linked, so the table deliberately does not claim a resolved
  runtime address.
- The **definition** is symbol 1 (`target`) inside section 1 at
  `st_value = 0x40` with `st_size = 8`. Its name is read from string
  table section 3 at offset 192 (an offset larger than the compressed
  string table's physical `sh_size`, but valid inside its 256-byte
  logical content).

### The extended section index (`SHN_XINDEX`)

A symbol's `st_shndx` field is only 16 bits. Symbol 1 carries the
escape value `SHN_XINDEX` (0xffff), so the real definition section is
not in the symbol itself. Section 4 is an `SHT_SYMTAB_SHNDX` table whose
`sh_link` points back at symbol table 2; entry 1 of that table holds the
actual section index 1. `gelf_getsymshndx()` therefore reports the raw
`SHN_XINDEX` while the companion data expands the definition section to
1. The relationship is driven entirely by section indices and
`sh_link`; compression of either table does not change it.

### Logical offsets vs. physical file ranges

- `logical_record_off` is the record's position in the *decompressed*
  section content (0 for the first record). It is the offset used by the
  GElf readers.
- `physical_scn_off` / `physical_scn_size` are the unchanged on-disk
  section header values: the whole physical container, compression
  header and zlib stream included.
- For the plain input the physical mapping is exact
  (`sh_offset + logical_record_off`). For the compressed input the
  mapping is marked `container-only:no-per-record-byte-mapping`: a
  deflate stream does not preserve byte boundaries, so it is false to
  claim that one decompressed record corresponds to a specific range of
  compressed file bytes. Only the container as a whole has a physical
  range.
- Because the containers differ in size, the two rows' physical columns
  differ (plain section 5: 24 bytes at 0x360; compressed section 5:
  44 bytes at 0x2b0 in the generated images), while all logical
  symbol-reference columns stay identical. `elf_rawfile`, the section
  header sizes/offsets/flags and `elf_rawdata` always describe these
  physical bytes; `elf_getdata` and `elf_strptr` describe the logical
  content.
