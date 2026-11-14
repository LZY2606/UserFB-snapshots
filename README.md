# RISC-V Disassembler

RISC-V Disassembler with support for RV32/RV64/RV128 IMAFDC

## Build instructions

```
cmake -B build
cmake --build build
```

## Static LR/SC retry-region analysis

Two read-only analysis layers sit on top of the production decode path;
input bytes are never executed.

- `rv_decode_canonical` (in `riscv-disas.h`) runs the shared
  opcode/operand/decompress pipeline and stops before pseudo-instruction
  lifting, returning the canonical op, rd/rs1/rs2, immediate and aq/rl.
  `disasm_inst` calls the same function and only adds lifting/formatting,
  so the analyzer never re-decodes bit fields or parses assembly text.
- `rv_code_block_decode` (`rv_struct_decode.h`) decodes a contiguous,
  little-endian RV64 I/A/C stream into self-contained records that keep the
  raw 16/32-bit length, PC, offset and full canonical decode. The block
  copies its input, so the source buffer may be released afterwards.
- `rv_lrsc_analyze` (`rv_lrsc.h`) recovers direct-jump/branch/fall-through
  control flow from the given entry PCs, tracks entry-register-relative
  addresses and SC success/failure status through ADDI/ADD/x0 copies (and
  their compressed forms), and reports one candidate per reachable LR.W/LR.D.

Each candidate carries the LR/SC/branch records, the contiguous region, the
proven failure (retry-to-LR) and success (leave-region) edges, a verdict of
`proven`, `unconstrained` or `unknown`, and the decisive instruction/edge PCs
behind every reason. Results own all data they expose; free them with
`rv_lrsc_result_free`. `unknown` means the static proof cannot be completed,
not that the code must fail at runtime.
