# AMD XOP support in diStorm

This note describes the AMD XOP (eXtended Operations) subset that diStorm can
now decode. Decoding only reads bytes; no machine code is ever executed and the
host CPU does not need to implement XOP. Results are produced through the
existing `distorm_decompose` / `distorm_decode` / `distorm_format` entry points
and land in the ordinary `_DInst` operand model — there is no separate
string-only decoder.

## Scope

Only 32 bit (`Decode32Bits`) and 64 bit (`Decode64Bits`) modes are affected.
16 bit mode (`Decode16Bits`) keeps its legacy behavior: `0x8F` is never treated
as an XOP prefix there.

| Instruction | Map / opcode | Vector length | Forms |
|-------------|--------------|---------------|-------|
| `VPCMOV` | map 8, `A2` | 128 and 256 bit | register or memory sources |
| `VPPERM` | map 8, `A3` | 128 bit only | register or memory sources |
| `VPROTB` | map 8 `90` / map 9 `C0` | 128 bit | vector count (map 8), imm8 count (map 9) |
| `VPROTW` | map 8 `91` / map 9 `C1` | 128 bit | vector count (map 8), imm8 count (map 9) |
| `VPROTD` | map 8 `92` / map 9 `C2` | 128 bit | vector count (map 8), imm8 count (map 9) |
| `VPROTQ` | map 8 `93` / map 9 `C3` | 128 bit | 64 bit mode only; vector and imm8 count |

No other XOP instructions are recognized. Reserved or unsupported encodings
fall through to the original invalid-byte (`DB`) handling.

## Encoding

The three-byte XOP prefix reuses the `0x8F` opcode byte:

```
8F | R~ X~ B~ mmmmm | W vvvv L pp | opcode | ModRM [SIB] [disp] | tail
```

* `mmmmm` selects the map. Only `8` and `9` are handled. A low-five value below
  `8` is the normal opcode space, so e.g. `8F /0` remains `POP` and is never
  swallowed as a prefix.
* `R~ X~ B~` are the inverted register/address extension bits, handled like the
  VEX equivalents (they feed `vrex` and extend ModRM.reg, ModRM.rm and SIB).
* `W`, `L`, `vvvv` are taken from the third byte. In 32 bit mode `W` is still
  honored: for these instructions it selects the operand layout, it is not a
  64 bit GPR width bit, so it must not be cleared outside 64 bit mode.
* `L` selects 128 (`L=0`) or 256 (`L=1`) vectors. `VPPERM` and all `VPROT*`
  reject `L=1`.

## Operand order and the W bit

Operands are always emitted in Intel order: destination first, then sources.

### VPCMOV / VPPERM

Both read a fourth vector register from the **high nibble of the trailing
byte**. That nibble is a register operand (`O_REG`), not a displayed immediate,
and the byte is consumed only after ModRM, SIB and any displacement so the
next instruction never gets misaligned.

`W` decides which of the two middle sources the ModRM byte supplies:

* `W=0`: `dest, vvvv, ModRM, tail-reg`
* `W=1`: `dest, ModRM, vvvv, tail-reg`

When ModRM is a memory operand it therefore moves to the matching semantic
source slot; the memory's base, index, scale, displacement, segment and
RIP-relative flag are all filled in the `_DInst`, not merely reflected in text.

### VPROTB/W/D/Q

The vector form (map 8) uses a vector count register; the immediate form
(map 9) keeps a genuine `O_IMM` imm8.

* Vector, `W=0`: `dest, ModRM(data/mem), vvvv(count reg)`
* Vector, `W=1`: `dest, vvvv(data), ModRM(count reg, register only)`
* Immediate (map 9, always `W=1`, `L=0`): `dest, ModRM(data/mem), imm8`

The count register's operand size follows the element width: 8, 16, 32 or 64
bits for `VPROTB/W/D/Q` respectively. The immediate form stores the count in
`di.imm` with an 8 bit `O_IMM` operand.

## Routing and invalid handling

* `0x8F` with `mmmmm < 8` decodes exactly as before (e.g. `POP r/m`).
* `0x8F` with `mmmmm = 8/9` but an unsupported opcode, or a reserved `W`/`L`
  combination (for example map 9 with `W=0`, or `VPPERM` with `L=1`), is not
  consumed as a valid instruction; the decoder resumes the legacy byte-by-byte
  invalid walk and never leaves a partially filled operand set.
* Truncated prefixes and byte-by-byte cuts produce single-byte `DB` entries and
  do not desynchronize or pollute the following valid instruction.

## Public interface compatibility

The additions reuse existing opcode ids space and register enums. New opcode
ids (`I_VPCMOV`, `I_VPPERM`, `I_VPROTB`, `I_VPROTW`, `I_VPROTD`, `I_VPROTQ`)
and their mnemonic strings are appended; no existing value or public structure
layout changes. Every decoded instruction sets `FLAG_DST_WR` on its writable
destination, an accurate `usedRegistersMask`, and the normal memory fields, so
structured decomposition, the legacy text decoder and formatting all describe
the same instruction.

## Example

`examples/xop/xop_example.c` decodes static VPCMOV, VPPERM and VPROT byte arrays
through both `distorm_decompose` and `distorm_decode`. The contract cases in
`tests/xop_contract.c` cover routing, operand placement (including W-driven
swapping and real memory fields) and stream continuation/truncation.
