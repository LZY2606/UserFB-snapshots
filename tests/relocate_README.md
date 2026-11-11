# Relocation test cases

`relocate_cases.c` exercises the offline x86-64 relocator in
`include/relocate.h` / `src/relocate.c`. It is built and run by
`run_relocate.sh`, which compiles the real library sources (including
`src/relocate.c`) with the native C compiler in a temporary directory; nothing
is installed and no machine code under test is ever loaded or executed.

The script prints exactly one `PASS <group>` line per group and finishes with
`RELOCATE_CASES_OK cases=7`. Every expectation below is derived by hand from
the Intel/AMD encodings and from independent arithmetic in the test file; the
relocated bytes are never fed back into the relocator as their own proof.

## Groups

### 1. `field_spans`

Verifies that the field records of `distorm_decompose_fields64` describe the
exact bytes the decoder consumed, not guessed positions.

Original bytes:

- `48 C7 05 78 56 34 12 EF BE AD DE` — `MOV [RIP+0x12345678], 0xDEADBEEF`.
  The RIP disp32 is recorded at offset 3, width 4, target `base+11+0x12345678`
  (e.g. `0x12349683` at base `0x4000`). The following imm32
  `EF BE AD DE` is a separate field and must not be touched by a rewrite.
- `66 0F 3A 0F 05 F0 FF FF FF 07` — `PALIGNR` with RIP disp32 at offset 5 and
  the imm8 `07` at offset 9.
- `75 7C` — short `JNZ`, PC field at offset 1, width 1, target `base+0x7E`.
- `E8 00 00 00 00` — near `CALL`, PC field at offset 1, width 4.
- `90` — no fields, empty record.
- `90 F0` — the trailing undecodable byte still pairs 1:1 with an empty
  record, proving records do not carry over between entries or calls.

A mixed batch and a second interleaved decode call confirm that records stay
aligned with instructions and that `distorm_decompose` still returns the same
`_DInst` bytes.

### 2. `widening_cascade`

Uses the witness fragment (132 bytes): two internal short `Jcc`
(`75 7C`, `75 7F`) and an external near `JMP` (`E9 rel32 -> 0x200000`),
padded with NOPs and terminated by `C3`.

- A small slide changes only displacements: both short branches stay two
  bytes and the external `JMP` stays a five-byte `E9`; no padding is emitted.
- A slide past the 4 GiB boundary makes the external `JMP` unreachable by
  rel32, so it becomes the six-byte `FF 25 disp32` form. That one extra byte
  pushes the second internal `Jcc` from rel8 distance 127 to 128; it widens
  to `0F 85 rel32`, which in turn pushes the first `Jcc` out of range as
  well. The expected `0F 85` bytes and every rel32 value are recomputed in
  the test from the moved instruction addresses.
- A dedicated cascade case places an external target near the negative rel32
  edge (`INT32_MIN + 24`): encodable before a +256 slide, unencodable after
  it, forcing a short internal `JMP` (`EB 7F`) to widen purely because of the
  external transfer's growth.

### 3. `far_transfers`

16 bytes containing `JMP 0x4000`, `CALL 0x4000` and `JMP 0x8000`, moved to a
4 GiB-higher address where no rel32 direct transfer reaches.

- Each five-byte direct transfer becomes six bytes (`FF 25` / `FF 15`).
- The `JMP` and `CALL` sharing absolute target `0x4000` resolve to one shared
  eight-byte table entry; `0x8000` gets a distinct entry; the continuation gets
  a third (24 bytes total). Entry contents are read back as 64-bit values and
  compared with the expected absolute addresses.
- The `CALL` return successor is asserted to be the next relocated
  instruction, and the tail's indirect jump entry holds the original segment
  end address, so natural fall-through can never enter the constant region.

### 4. `loop_flags`

`LOOP`, `LOOPE`, `LOOPNE`, `JRCXZ` and their single-`67` ECX forms, with
self/short displacements.

- The optional `67` prefix, opcode byte and rel8-only encoding are preserved;
  these instructions have no register-free widened form.
- A restricted interpreter in the test file models only the counter width
  (RCX vs ECX with truncation and zero wrap), ZF and the instruction pointer.
  It steps both the original and the relocated streams over seven counter
  values (including `0`, `1`, `0xFFFFFFFF`, `0x100000001` and
  `0xFFFFFFFFFFFFFFFF`) and both ZF values, and requires identical taken /
  fall-through decisions, identical counter results and matching moved
  targets. No machine code is executed.
- A `LOOP` whose target is pushed beyond rel8 by an external transfer's
  widening is reported as `RLOC_OVERFLOW` at the branch's original offset,
  with no partial output.

### 5. `rip_data`

- The witness `MOV` and `PALIGNR` patterns: after relocation the RIP disp32 is
  rewritten at the recorded span so the effective data address is unchanged,
  while the trailing imm32 / imm8 bytes are compared byte-for-byte against the
  original.
- A RIP reference whose data address lies inside the original code region is
  still treated as data: the effective address remains the *old* address and
  is never remapped through the instruction mapping.
- A move that makes a data address unreachable by disp32 fails with
  `RLOC_RIP_UNREACHABLE` and no output; no register-based workaround is used.
- Plain instructions are copied verbatim. An indirect `JMP` (`FF 25`, no
  `O_PC`) is not treated as a direct branch: its RIP data address is preserved
  and no tail-table entry is created.

### 6. `diagnostics`

- Truncated instruction head (`48 C7 05`) and an undecodable tail report
  `RLOC_UNDECODABLE` with the offending original offset and leave no owned
  output.
- A direct branch landing in the middle of a multi-byte instruction reports
  `RLOC_BAD_TARGET` at the branch offset.
- A wrapping original range reports `RLOC_OVERFLOW`.
- Explicitly unsupported constructs report `RLOC_UNSUPPORTED`: a CS-prefixed
  `JMP` (`2E EB 00`), an operand-size-prefixed near branch (`66 E9`, rel16),
  `XBEGIN` (`C7 F8 rel32`) and 32-bit address-size memory addressing
  (`67 8B 05 ...`).
- Empty input yields an empty successful result; freeing it repeatedly is
  safe. Null arguments are rejected with `RLOC_INPUT`.

### 7. `abi_reuse`

- Two different buffers are decoded with `distorm_decompose_fields64` in an
  interleaved A-B-A order; the second A result must match the first and must
  not contain B's records.
- The `_DInst` stream matches the unchanged `distorm_decompose` output, and
  `distorm_decode` still formats `0F 01 CB` as `STAC` / `0f01cb`.
- A failed relocation followed by a successful one reuses the same result
  struct; the input buffer is checksummed before and after to prove it is
  untouched, and freeing is idempotent.

## Still unsupported (reported, never silently mis-encoded)

- Direct branches carrying any prefix other than a single `67` on the LOOP /
  JECXZ family: segment overrides, LOCK/REP, operand-size (rel16) and REX/VEX.
- `XBEGIN`, far direct `CALL`/`JMP`, and 32-bit address-size memory operands.
- LOOP-family branches that cannot stay within rel8 in the stabilized layout
  (no register-free widening exists for them).
- RIP-relative data references that cannot keep their original data address
  within a disp32 after the move.
- Position-self-inspecting patterns such as `CALL`/`POP` used to read the
  current instruction pointer.
