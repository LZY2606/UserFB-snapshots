# RISC-V reference analysis

This note accompanies `analysis/riscv-references.tsv`. The TSV is produced by
the C++17 consumer in `examples/riscv_reloc_analysis/report.hpp`
(`ELFIO::riscv_example::build_riscv_reference_report` followed by
`write_riscv_reference_report`). The consumer calls the production entry point
`ELFIO::analyze_riscv_relocations()` from `elfio/elfio_riscv_reloc.hpp` and then
**re-decodes** the referenced target from the machine words that entry point
returns. It does not copy the target out of the plan.

No linker, external disassembler, assembler, machine-code loader or simulator
was used. The two input objects (`analysis/witness-saved.elf` and
`analysis/cross-table-saved.elf`) were built with ELFIO itself, saved, reloaded
and saved again; the expected results below are derived by hand from the
machine words and the symbol fields.

## 1. Why the original immediate is not the answer

Consider the witness object. Its `.text` (physical section index 2, file offset
`0x6c`, 12 bytes) is:

```
0x6c: 17 05 00 00   auipc a0, 0
0x70: 13 05 05 00   addi  a0, a0, 0
0x74: 23 20 15 00   sw    a0, 0(a0)
```

The encoded immediates are zero. If one read only those words before
relocation, one would conclude that `auipc a0, 0` computes the address of its
own instruction and that `addi a0, a0, 0` adds nothing — i.e. a reference to
`0x6c`-relative address zero. That is wrong in both layouts:

* at `.text = 0x1000` the relocated pair makes `a0 = 0x1800`;
* at `.text = 0x2000` the relocated pair still makes `a0 = 0x1800`, although
  the patched `auipc` immediate is again `0`.

The referenced identity lives in the relocation records, not in the unrelocated
immediate fields. Only after the relocation is applied and the two halves are
combined does the meaning become visible.

## 2. Why the low record points at the high anchor

The witness `.rela.text` (physical section index 5, file offset `0xd1`, entry
size 24) holds three entries:

| entry | file offset | bytes | meaning |
|---|---|---|---|
| 0 | `0xd1` (209) | `04 00 00 00 00 00 00 00  18 00 00 00 01 00 00 00  00…` | `r_offset=4`, symbol 1 `.Lanchor`, type 24 = `PCREL_LO12_I`, addend 0 |
| 1 | `0xe9` (233) | `00 00 00 00 00 00 00 00  17 00 00 00 02 00 00 00  00…` | `r_offset=0`, symbol 2 `target`, type 23 = `PCREL_HI20`, addend 0 |
| 2 | `0x101` (257) | `00 00 00 00 00 00 00 00  33 00 00 00 00 00 00 00  00…` | `r_offset=0`, symbol 0, type 51 = `RELAX` |

The symbol table (physical index 4, file offset `0x89`, entry size 24) holds:

| symbol | file offset | bytes | fields |
|---|---|---|---|
| 1 `.Lanchor` | `0xa1` (161) | `01 00 00 00 00 00 02 00  00…` | `st_name=1`, `st_info=0x00`, `st_shndx=2` (`.text`), `st_value=0` |
| 2 `target` | `0xb9` (185) | `0a 00 00 00 11 00 f1 ff  00 18 00 00 00 00 00 00  00…` | `st_name=10`, `st_info=0x11` (GLOBAL OBJECT), `st_shndx=0xfff1` (`SHN_ABS`), `st_value=0x1800` |

Entry 0 is recorded **before** entry 1 and is not adjacent to it. Its symbol is
`.Lanchor`, not `target`. Its `st_shndx` is the code section (2) and its
`st_value` is 0: that pair of fields is the anchor identity. The unique
`PCREL_HI20` record at section 2 / offset 0 is the matching high half. The table
name `.rela.text` is irrelevant; only `sh_link` (4, the symbol table) and
`sh_info` (2, the code section) matter.

The low relocation deliberately refers to the **high instruction's position**,
not to the final target. A `PCREL_LO12` in RISC-V is defined relative to the
AUIPC it annotates: the linker looks up the AUIPC's symbol and addend and
splits the same `D = S + A - P` into high and low halves. The low instruction
must therefore not use its own PC.

Concretely, if one wrongly used the low instruction's PC `0x1004`, base
`0x1000`, one would compute `0x1004 + 0x1000 - 0x800 = 0x1804`, four bytes past
the real target. Using the anchor PC `0x1000` gives `0x1000 + 0x1000 - 0x800 =
0x1800`, the correct `target`.

## 3. Recovering the symbol relationship

For each high/low pair the production entry point computes

```
D   = S + A - P          (P is the AUIPC's PС)
hi  = floor((D + 0x800) / 0x1000)      // AUIPC bits 31:12
lo  = D - hi * 0x1000                  // signed 12-bit low half
```

It writes `hi` into AUIPC bits 31:12 and `lo` into bits 31:20 (I-type), the
split 31:25 / 11:7 fields (S-type), or the JALR immediate for `CALL`. It then
verifies that the sign-extended high part times `0x1000` plus the
sign-extended low part reproduces `D` exactly; otherwise it refuses rather than
truncating to 32 bits. The consumer independently re-decodes the returned
words as `P + sext20(hi) * 0x1000 + sext12(lo)` and requires the result to
equal the target symbol address plus addend.

## 4. Witness object at `.text = 0x1000`

`D = 0x1800 + 0 − 0x1000 = 0x800`, so `hi = 1`, `lo = -0x800`.

* AUIPC at `0x1000` (original `0x00000517`, file `0x6c`): bits 31:12 become 1,
  patched word `0x00001517`. `P + 0x1000 = 0x2000`.
* ADDI at `0x1004` (original `0x00050513`, file `0x70`): bits 31:20 become
  `-0x800`, patched word `0x80050513`. `0x2000 − 0x800 = 0x1800`.
* The store word at `0x74` is untouched: `0x00152023`.

The report row shows `symidx=2` (`target`), `sym_addr=6144 = 0x1800`,
`referenced_target=0x00001800` for the high record; the low row shows its own
symbol as `.Lanchor` (`symidx=1`, section-relative value `0`), the matched
anchor `hi_rela_section=5, hi_rela_entry=1`, and `hi_symidx=2`
(`target`, `0x1800`). The low row's decoded `imm` is `-0x0800` and its
`referenced_target` is `0x00001800`. The RELAX entry is listed separately with
`main_rela_entry=1`; it does not create a second reference.

## 5. Witness object at `.text = 0x2000`

`D = 0x1800 + 0 − 0x2000 = -0x800`, so `hi = 0`, `lo = -0x800`.

* AUIPC at `0x2000` (original `0x00000517`, file `0x6c`): `hi = 0`, so the word
  is unchanged, `0x00000517`. The decoded immediate is `+0x0000`.
* ADDI at `0x2004` (original `0x00050513`, file `0x70`): `lo = -0x800`, patched
  `0x80050513`, identical to the `0x1000` case.
* Store: `0x00152023` again.

Comparing the two layouts in the TSV: the `pc` column moves `4096 → 8192`, the
AUIPC decoded `imm` changes `+0x1000 → +0x0000`, and the AUIPC `patched_word`
changes `0x00001517 → 0x00000517`. The low immediate `-0x0800` and the
`referenced_target 0x00001800` stay put, and both rows keep `symidx=2` and
`sym_addr=6144`. The target is anchored to the `SHN_ABS` symbol `target` in
both layouts, while the physically patched bytes differ.

## 6. Cross-table shared high record at `.text = 0x1000`

The second object's `.text` (index 2, file `0x76`, 16 bytes) is:

```
0x76: 97 02 00 00   auipc t0, 0
0x7a: 93 82 02 00   addi  t0, t0, 0
0x7e: 03 a3 02 00   lw    t1, 0(t0)
0x82: 23 a0 62 00   sw    t1, 0(t0)
```

Sections 5 (`.rela.one`, file `0xe2`, size 24) and 6 (`.rela.two`, file
`0xfa`, size 72) both have `sh_link=4` (`.symtab`) and `sh_info=2` (`.text`).
Their names do not participate in matching.

* `.rela.one` entry 0 at `0xe2`: `r_offset=0`, `r_info` type 23, symbol 2.
* `.rela.two` entry 0 at `0xfa`: `r_offset=12`, type 25 (`PCREL_LO12_S`),
  symbol 1 (`.Lhi`, `st_shndx=2`, `st_value=0`).
* `.rela.two` entry 1 at `0x112`: `r_offset=8`, type 24 (`PCREL_LO12_I`),
  symbol 1.
* `.rela.two` entry 2 at `0x12a`: `r_offset=4`, type 24 (`PCREL_LO12_I`),
  symbol 1.

The symbol table (index 4, file `0x9a`) has symbol 2 `shared_target` at file
`0xca` with bytes `06 00 00 00 11 00 f1 ff bc 1a 00 00 …`: `st_shndx=0xfff1`
(`SHN_ABS`), `st_value=0x1abc`.

Here `D = 0x1abc − 0x1000 = 0xabc`, so `hi = 1`, `lo = -0x544`.

* AUIPC at `0x1000` (original `0x00000297`): patched `0x00001297`.
* ADDI at `0x1004` (original `0x00028293`): patched `0xabc28293`.
* LW at `0x1008` (original `0x0002a303`): patched `0xabc2a303`.
* SW at `0x100c` (original `0x0062a023`): patched `0xaa62ae23`. The low bits
  are split: bits 31:25 take `(-0x544 >> 5) & 0x7f = 0x55`, bits 11:7 take
  `-0x544 & 0x1f = 0x1c`; the `rs2` (t1), `rs1` (t0), `funct3` (2 = SW) and
  opcode (0x23) are preserved, so the patched word is
  `0x55<<25 | 6<<20 | 5<<15 | 2<<12 | 0x1c<<7 | 0x23 = 0xaa62ae23`.

All three low rows resolve to the **same** anchor record (`.rela.one` entry 0,
`hi_pc = 4096`) even though two of them live in a different table and none is
adjacent to the high record. Every low row in the TSV reports
`hi_symidx=2` / `hi_sym_addr=6844` (`0x1abc`) and `referenced_target=0x00001abc`,
which matches `target + addend`. The high row itself carries
`referenced_target=0x00001abc` as well, decoded from its own returned AUIPC word
plus the low half taken from a paired low record.

## 7. What is not done

This is a relocation-level reference recovery, not a disassembler. It does not
widen or shrink code, does not handle `PLT`/`GOT` or TLS, and does not simulate
execution. `R_RISCV_NONE` writes nothing. `R_RISCV_RELAX` is reported once as a
hint row that names the main record it relaxes; it is neither a second write nor
a second reference, and it never changes an instruction's length. Any
unrepresentable 64-bit value, out-of-section write, broken table, bad symbol
index, or ambiguous/missing anchor is reported with the RELA table index and
entry, and no plan is published.
