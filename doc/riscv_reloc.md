# Offline RISC-V Relocation Planning

ELFIO can turn the RISC-V relocation records in an unlinked (ET_REL) object
file into usable, relocated instruction bytes without invoking a linker and
without executing any machine code. The feature lives in the header-only
file `elfio/elfio_riscv_reloc.hpp`, which is pulled in automatically by
`#include <elfio/elfio.hpp>`. It uses only the C++17 standard library and has
no third-party dependencies.

## When to use it

In a RISC-V object file a single symbol reference is normally expressed as
two records: an `R_RISCV_PCREL_HI20` on an `AUIPC` instruction holding the
high 20 bits, and one or more `R_RISCV_PCREL_LO12_I` /
`R_RISCV_PCREL_LO12_S` records on the instruction that consumes the low 12
bits. A disassembler cannot read meaningful code from such an object until
those pairs are resolved against concrete section addresses.
`make_riscv_reloc_plan()` performs exactly that resolution and returns
independently owned output bytes together with a per-instruction change log.

The input object and the caller's base-address table are never modified.
Bits not covered by a relocation are preserved bit-for-bit, and no section
other than the relocated executable sections receives an output copy.

## Supported inputs

- ELF class / data: **ELFCLASS64**, **ELFDATA2LSB** (64-bit, little endian).
- Machine / type: **EM_RISCV**, **ET_REL**.
- Target sections: executable, allocated `SHT_PROGBITS` sections
  (`SHF_ALLOC | SHF_EXECINSTR`) containing uncompressed (4-byte)
  instructions.
- Relocation tables: `SHT_RELA` only, matching the RISC-V psABI. Each table
  is resolved with its own `sh_link` symbol table and located with its own
  `sh_info` code section; section names do not influence any decision.

## Supported relocations

| Type | Instruction requirement | Effect |
| --- | --- | --- |
| `R_RISCV_PCREL_HI20` (23) | `AUIPC` | Writes bits 31:12 of `D = S + A - P` |
| `R_RISCV_PCREL_LO12_I` (24) | `ADDI`, integer load or `JALR` | Writes bits 31:20 (signed low 12 of `D`) |
| `R_RISCV_PCREL_LO12_S` (25) | Integer store | Splits the signed low 12 across bits 31:25 and 11:7 |
| `R_RISCV_CALL` (18) | Consecutive `AUIPC` + `JALR` | Patches both words using the first instruction's PC |
| `R_RISCV_NONE` (0) | — | Recognized; writes nothing |
| `R_RISCV_RELAX` (51) | — | Hint only; writes nothing and never changes instruction length |

Anything else (for example branch relocations, `R_RISCV_32`/`R_RISCV_64`
integer data writes, TLS, GOT or PLT entries, or the relax/align sequence
types) is rejected. Integer data writes are deliberately not treated as
instruction relocations. Branch relaxation, code shortening, PLT, GOT and TLS
are out of scope.

## Address model

- `base_addresses[section_index]` gives the physical load address of every
  section index the planner needs. A needed index that has no entry is an
  error.
- An ordinary defined symbol resolves to the base address of its section
  plus its `st_value`.
- `SHN_ABS` symbols resolve to their `st_value` directly.
- Undefined (`SHN_UNDEF`), `SHN_COMMON`, reserved and extended
  (`SHN_XINDEX` and other `SHN_LORESERVE`..`SHN_HIRESERVE` except `SHN_ABS`)
  symbol section indices are rejected. Symbols are never merged by name.

## High/low pairing

The low-part record references an **anchor** symbol that labels the high
`AUIPC`. The planner finds the unique matching `R_RISCV_PCREL_HI20` using the
anchor's physical section identity and `st_value` (the AUIPC offset):

- low parts may appear before their high part, and several low parts may
  share one high part;
- the low and high records may live in different RELA tables;
- pairing never depends on entry adjacency or symbol names;
- the anchor must be defined in the same code section as the low-part
  instruction; an anchor that matches zero or more than one high part is an
  error.

The anchor is not the eventual target, and the low-part displacement is
never computed from the low instruction's own PC. Low-part addends must be
zero; the high-part addend participates normally in `D = S + A - P`, where
`P` is the actual address of the `AUIPC` (section base plus its offset).

The carry from the signed low 12 bits is folded into the high part. After
patching, the sign-extended AUIPC high value plus the sign-extended low
value reconstructs `D` exactly; a displacement that cannot be represented is
rejected rather than truncated to 32 bits.

## CALL handling

`R_RISCV_CALL` acts on two consecutive words, `AUIPC` followed by `JALR`, and
uses the PC of the first word. The `AUIPC` destination register must feed the
`JALR` base register; destination registers, base registers and all
non-immediate fields of both words are preserved.

## RELAX and NONE hints

`R_RISCV_RELAX` is only accepted at an offset that already carries a
supported primary relocation. It never causes a second write and never
changes an instruction length. `R_RISCV_NONE` is ignored entirely. A RELAX
record without a matching primary record is an error.

## Validation and failure semantics

Planning is all-or-nothing: on any error no partial plan is published and the
returned `riscv_reloc_plan` is left untouched. Every error message names the
responsible relocation **table index and entry position**. Failures include,
among others:

- truncated relocation records or an undersized relocation entry size;
- out-of-range or wrong-typed `sh_link` / `sh_info`;
- symbol indices outside the table's own symbol table;
- missing, non-unique or cross-section low-part anchors;
- writes that would fall outside the target section;
- two active relocations modifying the same instruction word (a RELAX hint
  on the same offset is not a conflict);
- unsupported relocation types or unsuitable instruction opcodes;
- address or displacement overflow, including unrepresentable HI20/LO12
  pairs.

## API

```cpp
namespace ELFIO {

enum class riscv_reloc_kind {
    pcrel_hi20, pcrel_lo12_i, pcrel_lo12_s, call
};

class riscv_reloc_change {
  public:
    riscv_reloc_kind kind;
    Elf_Half   reloc_section_index; // source RELA table index
    Elf_Xword  reloc_entry_index;   // entry position inside that table
    Elf_Half   code_section_index;  // patched code section index
    Elf64_Addr code_offset;         // offset of the patched word
    Elf64_Addr high_offset;         // offset of the matching AUIPC
    std::uint32_t original_word;    // word before patching
    std::uint32_t patched_word;     // word after patching
};

class riscv_reloc_section {
  public:
    Elf_Half                        section_index;
    std::vector<unsigned char>      data;    // owned, relocated bytes
    std::vector<riscv_reloc_change> changes; // changes applied to data
};

class riscv_reloc_plan {
  public:
    std::vector<riscv_reloc_section> sections;
    std::vector<riscv_reloc_change>  changes; // stable global order
};

// Returns an empty string on success, otherwise an error message.
std::string make_riscv_reloc_plan(
    const elfio&                    elf_file,
    const std::vector<Elf64_Addr>&  base_addresses,
    riscv_reloc_plan&               plan );

} // namespace ELFIO
```

`plan.sections` is ordered by original section index and contains only code
sections with at least one active relocation. `plan.changes` has a stable
order (code section index, then code offset, then table index, then entry
index) that does not depend on section-table traversal order. A `CALL`
produces two changes; `NONE` and `RELAX` produce none.

## Minimal usage

```cpp
#include <elfio/elfio.hpp>
using namespace ELFIO;

elfio reader;
reader.load( "firmware.o" );

std::vector<Elf64_Addr> bases( reader.sections.size(), 0 );
bases[text_section_index]   = 0x10000;
bases[rodata_section_index] = 0x80000;

riscv_reloc_plan plan;
std::string error = make_riscv_reloc_plan( reader, bases, plan );
if ( !error.empty() ) {
    // error already names the table index and entry position
    return;
}

for ( const riscv_reloc_section& out : plan.sections ) {
    // out.data is an independent buffer the caller may hand to a
    // disassembler; the original object is unchanged.
}
```

A complete, self-contained program that builds a small object and plans
HI20/LO12 (I- and S-type) plus CALL relocations is provided in
`examples/riscv_reloc/riscv_reloc.cpp`.
