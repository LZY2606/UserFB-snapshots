# AVX2 VEX Gather (VSIB) Decoding

## Scope

udis86 decodes the byte stream of the eight AVX2 VEX gather instructions in
their VEX.128 and VEX.256 memory forms, in both 32-bit and 64-bit
disassembly modes:

| Mnemonic    | VEX.66.0F38 | VEX.W | Element | Destination / mask | Index vector |
|-------------|-------------|-------|---------|--------------------|--------------|
| VPGATHERDD  | 90          | 0     | dword   | VEX.L              | VEX.L        |
| VPGATHERDQ  | 90          | 1     | qword   | VEX.L              | VEX.L'       |
| VPGATHERQD  | 91          | 0     | dword   | VEX.L'             | VEX.L        |
| VPGATHERQQ  | 91          | 1     | qword   | VEX.L              | VEX.L        |
| VGATHERDPS  | 92          | 0     | dword   | VEX.L              | VEX.L        |
| VGATHERDPD  | 92          | 1     | qword   | VEX.L              | VEX.L'       |
| VGATHERQPS  | 93          | 0     | dword   | VEX.L'             | VEX.L        |
| VGATHERQPD  | 93          | 1     | qword   | VEX.L              | VEX.L        |

VEX.L selects 128-bit (`L=0`, XMM) or 256-bit (`L=1`, YMM) operation.
The primed column marks the split-width forms whose index vector does not
track VEX.L:

- 256-bit VGATHERDPD / VPGATHERDQ: destination and mask are YMM, but the
  index is XMM.
- 256-bit VGATHERQPS / VPGATHERQD: destination and mask are XMM, but the
  index is YMM.

Only decoding is performed. Instructions are never executed, no register
snapshot is read, and no per-lane effective addresses are computed. EVEX
encodings and VEX/EVEX scatter instructions are out of scope.

## Operand model

The Intel operand order is destination, VSIB memory, vector mask, and the
structured `ud_operand` array follows the same order:

- `ud_insn_opr(u, 0)` - destination vector register (`UD_OP_REG`,
  XMM/YMM enum, size 128 or 256), from the ModRM.reg field plus VEX.R.
- `ud_insn_opr(u, 1)` - the VSIB memory operand (`UD_OP_MEM`). Its `size`
  is the width of a single gathered data element, 32 or 64 bits, not the
  width of the whole vector.
- `ud_insn_opr(u, 2)` - mask vector register (`UD_OP_REG`, same XMM/YMM
  width as the destination), from the inverted VEX.vvvv field.

## VSIB field meaning

A VSIB address rides the ordinary ModRM/SIB byte sequence, so it enters
through the same ModRM/SIB path as a scalar memory operand; the
differences are confined to the SIB interpretation:

- The SIB index field names a vector register. Index value 4 therefore
  means `xmm4`/`ymm4` and is **not** the scalar "no index" marker. The
  vector register's width comes from the instruction encoding (the table
  above), never inferred from VEX.L alone.
- VEX.X (surfaced internally as the REX.X bit) extends the vector index
  number; VEX.B (REX.B) extends the base register number. In 64-bit mode
  the extension bits therefore reach xmm8-xmm15/ymm8-ymm15 and r8-r15 for
  the index and base respectively.
- The SIB base field and the ModRM displacement keep their normal
  general-register meaning: the base is always a GPR (or absent).
- Scale, displacement width and the signed displacement value use the
  existing `ud_operand` fields (`scale`, `offset`, `lval`).
- A segment override prefix is reported through `pfx_seg` as usual, and
  the 67h address-size override keeps its normal effect: 32-bit
  addressing in 64-bit mode, 16-bit addressing in 32-bit mode.
- With ModRM.mod == 0 and an SIB base of rbp/r13 (64-bit) or ebp (32-bit)
  there is no base register and a disp32 follows. Because a SIB byte is
  present this is a plain no-base address, never RIP-relative; `base` is
  `UD_NONE` and never `UD_R_RIP`.

Examples:

```
vgatherdps xmm1, dword [rax+xmm4*4+0x10], xmm2
vgatherdpd ymm3, qword [r13+xmm12*8-0x20], ymm5
vgatherqpd ymm9, qword [ymm10*4+0x11223344], ymm11
```

AT&T syntax renders the same three operands in reverse order and prints a
base-less vector index without a base register:

```
vgatherdps %xmm2, 0x10(%rax,%xmm4,4), %xmm1
vgatherqpd %ymm11, 0x11223344(%ymm10,4), %ymm9
```

## Validity rules

The destination, vector index and mask must name three distinct physical
vector registers. XMM and YMM views sharing a register number alias the
same physical register, so e.g. a YMM destination with an XMM index of the
same number is a conflict and decodes to `UD_Iinvalid`.

The following also remain in the existing invalid state:

- no SIB byte (ModRM.rm != 4);
- register form (ModRM.mod == 3);
- 16-bit addressing, which has no SIB/VSIB encoding;
- any truncated input that ends before the full ModRM/SIB/disp sequence.

Ordinary scalar SIB decoding is unchanged: outside a gather, SIB index
field 4 still means "no index".
