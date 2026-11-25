# AVX2 gather / VSIB decoding

This document describes the support for the AVX2 gather instructions added
to `udis86`. Decoding is byte-only: instructions are never executed,
register snapshots are not read and per-lane addresses are not computed.

## Scope

The following eight mnemonics are supported in their VEX memory forms, both
`VEX.128` and `VEX.256` (when architecturally meaningful), in 32-bit and
64-bit disassembly modes:

| Mnemonic      | Opcode map    | Element data size |
|---------------|---------------|-------------------|
| `vgatherdps`  | `VEX.66.0F38 92 /r`, `W=0` | 32-bit (`ps`) |
| `vgatherdpd`  | `VEX.66.0F38 92 /r`, `W=1` | 64-bit (`pd`) |
| `vgatherqps`  | `VEX.66.0F38 93 /r`, `W=0` | 32-bit (`ps`) |
| `vgatherqpd`  | `VEX.66.0F38 93 /r`, `W=1` | 64-bit (`pd`) |
| `vpgatherdd`  | `VEX.66.0F38 90 /r`, `W=0` | 32-bit |
| `vpgatherdq`  | `VEX.66.0F38 90 /r`, `W=1` | 64-bit |
| `vpgatherqd`  | `VEX.66.0F38 91 /r`, `W=0` | 32-bit |
| `vpgatherqq`  | `VEX.66.0F38 91 /r`, `W=1` | 64-bit |

For the integer gathers the opcode selects the index element width
(`90` = dword index, `91` = qword index) while `VEX.W` selects the data
element width, mirroring the floating-point gather encodings.

Out of scope: EVEX encodings and the scatter instructions
(`vscatter*`, opcodes `A0`/`A1`). Those bytes keep decoding as `invalid`.

## Operand order and structured result

Both the Intel text and `ud_insn_opr()` use the order

1. destination vector register,
2. VSIB memory operand,
3. vector mask register (`VEX.vvvv`).

For example `C4 E2 69 92 4C A0 10` decodes to

```
vgatherdps xmm1, dword [rax+xmm4*4+0x10], xmm2
```

with:

- `ud_insn_opr(u, 0)`: `UD_OP_REG`, `base == UD_R_XMM1`, `size == 128`;
- `ud_insn_opr(u, 1)`: `UD_OP_MEM`, the VSIB address;
- `ud_insn_opr(u, 2)`: `UD_OP_REG`, `base == UD_R_XMM2`, `size == 128`.

For the memory operand:

- `size` is the width of a **single gathered data element**, either `32`
  or `64`; it is deliberately not the full XMM/YMM vector width;
- `index` is an `UD_R_XMMn`/`UD_R_YMMn` enumerator (the VSIB index);
- `base` is a general-purpose register enumerator (`UD_R_RAX`, `UD_R_R13`,
  `UD_R_ECX`, ...) or `UD_NONE` for a base-less encoding;
- `scale`, `offset`, `lval`, the segment prefix (`u->pfx_seg`) and the
  effective address width (`u->adr_mode`) keep their existing meanings.
  `offset` is the displacement width in bits (`8`/`16`/`32`), and the
  signed value is read from `lval.sbyte`/`lval.sword`/`lval.sdword`
  according to that width, exactly as for ordinary SIB operands.

## Width map

The vector widths are not a pure function of `VEX.L`. The decoder applies
the per-flavour matrix below (`L=0` is always XMM for all three vectors).

| Mnemonic     | `L=1` destination / mask | `L=1` VSIB index | Memory `size` |
|--------------|--------------------------|------------------|---------------|
| `vgatherdps` | `YMM` / 256             | `YMM` / 256      | 32 |
| `vgatherdpd` | `YMM` / 256             | **`XMM` / 128**  | 64 |
| `vgatherqps` | **`XMM` / 128**         | `YMM` / 256      | 32 |
| `vgatherqpd` | `YMM` / 256             | `YMM` / 256      | 64 |
| `vpgatherdd` | `YMM` / 256             | `YMM` / 256      | 32 |
| `vpgatherdq` | `YMM` / 256             | **`XMM` / 128**  | 64 |
| `vpgatherqd` | **`XMM` / 128**         | `YMM` / 256      | 32 |
| `vpgatherqq` | `YMM` / 256             | `YMM` / 256      | 64 |

The two asymmetric cases called out by the architecture are therefore:

- 256-bit `vgatherdpd` / `vpgatherdq`: YMM destination and mask, but an
  XMM VSIB index (four 32-bit index elements for four 64-bit lanes);
- 256-bit `vgatherqps` / `vpgatherqd`: XMM destination and mask, but a YMM
  VSIB index (four 64-bit index elements for four 32-bit lanes).

## VSIB field semantics

VSIB reuses the normal ModRM/SIB path (`decode_modrm_rm()` in
`libudis86/decode.c`) with VSIB handling enabled for the gather operand:

- the **SIB index field** (`SIB.I`, bits 5:3) selects a vector register,
  not a GPR;
- index encoding `4` therefore means **`xmm4`/`ymm4`** and is never the
  "no index" marker that ordinary SIB uses for `rsp`/`esp`;
- in 64-bit mode the VEX `X` bit (the complement of `VEX.X`) extends the
  index to `xmm8`–`xmm15` / `ymm8`–`ymm15`;
- the VEX `R` bit extends the ModRM `reg` field (destination) and the VEX
  `B` bit extends the SIB/`rm` base to the high registers;
- `SIB.S` is the usual scale shift; the reported `scale` follows the
  existing udis86 convention (`2`, `4`, `8`, and `0` for a `*1` encoding);
- SIB base encoding `5` with `mod=00` is the standard base-less
  `disp32` form (`base == UD_NONE`). It is never misread as RIP-relative:
  RIP-relative addressing only applies to the direct `rm=101` form
  without a SIB byte;
- positive/negative `disp8` and `disp32` are decoded as before;
- `67h` selects 32-bit addressing in 64-bit mode (`u->adr_mode == 32`,
  `ecx`-style bases, while VEX extension bits can still select
  `xmm8+`/`r8d+` registers).

A gather requires a real VSIB memory encoding. The following decode as
`UD_Iinvalid` through the existing error state (operands are cleared and
the consumed byte count is still returned):

- `mod == 11b` (register `r/m` instead of memory);
- no SIB byte (`modrm.rm != 100b`);
- 16-bit address forms (`u->adr_mode == 16`);
- truncated byte streams;
- encodings where the destination, VSIB index and mask are not three
  pairwise distinct **physical** vector registers. `xmmn` and `ymmn` name
  the same physical register `n`, so an `xmm4` index colliding with a
  `ymm4` destination or mask is rejected.

## Syntax output

Both built-in translators print the same decoded operands.

- Intel keeps destination / memory / mask order and uses the normal
  printer, including the `dword`/`qword` element-size cast that follows
  `ud_operand.size`:
  `vgatherqpd ymm9, qword [ymm10*4+0x11223344], ymm11`.
- AT&T reverses the operand order as usual:
  `vgatherqpd %ymm11, 0x11223344(,%ymm10,4), %ymm9`.
  A base-less VSIB index prints the empty base slot `(,%ymm10,4)` so the
  vector register is unambiguous as an index. Ordinary GPR SIB printing
  is unchanged.

## Public interface notes

No new disassembly entry point was added; `ud_disassemble()`,
`ud_insn_opr()` and `ud_insn_asm()` are the interface. The eight new
`UD_Ivgather*` / `UD_Ivpgather*` mnemonic enumerators were appended after
`UD_Ipause`, so all pre-existing public mnemonic numeric values are
unchanged (`UD_Iinvalid` remains `903` in the supplied generated table).
A new internal `OP_VSIB` operand code was likewise appended after
`OP_MR`.

## Regeneration / files

The build used by `task_support/run.sh` compiles the checked-in generated
tables in `task_support/generated/libudis86/itab.c` and `itab.h`
directly; Python is not run during the build. Relevant changes:

- `task_support/generated/libudis86/itab.{c,h}` — mnemonics, strings and
  the `VEX.66.0F38 90h..93h` entries (`ud_itab` indices 1761–1764);
- `libudis86/decode.h` — internal `OP_VSIB` code;
- `libudis86/decode.c` — VSIB-aware ModRM/SIB decoding and the
  gather-specific flavour/width/uniqueness fixup;
- `libudis86/syn-att.c` — base-less vector-index `(,...)` printing;
- `tests/gather_contract.c` — the `width_map`, `vsib` and `compat`
  `task_cases` registered through `task_support/check.h`.
