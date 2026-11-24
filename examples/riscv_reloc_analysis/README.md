# RISC-V relocation reference report

`report.hpp` is a C++17 consumer that takes a `const ELFIO::elfio&`, a
`ELFIO::riscv_section_base_table` and a `std::ostream&`. It calls the production
entry point `ELFIO::analyze_riscv_relocations()` from
`elfio/elfio_riscv_reloc.hpp` and then decodes the referenced target from the
returned machine words (AUIPC upper immediate + sign-extended low immediate +
the AUIPC's own PC). It emits tab-separated rows with the source RELA entry,
symbol entry and code word, plus their physical file offsets in the input ELF.

The same header is included by `tests/riscv_reloc_cases.cpp`; it has no
dependency beyond the C++17 standard library and the repository sources.

## Build

```sh
c++ -std=c++17 -I. examples/riscv_reloc_analysis/main.cpp -o .task-build/riscv_reloc_analysis
```

## Run

The program takes an ELF path followed by one or more `<section-index>=<base>`
pairs. Every section needed by a relocation or symbol must be given a base; the
section index is the physical index (`sh_link`/`sh_info` index), not a name.

```sh
.task-build/riscv_reloc_analysis analysis/witness-saved.elf 2=0x1000
.task-build/riscv_reloc_analysis analysis/witness-saved.elf 2=0x2000
.task-build/riscv_reloc_analysis analysis/cross-table-saved.elf 2=0x1000
```

The report goes to stdout. `RELAX` records appear once as hint rows naming the
main record they relax; they never produce a second instruction write or a
fabricated reference target.
