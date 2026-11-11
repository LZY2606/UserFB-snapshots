# native_width — bddisasm/bdshemu width, flag and addressing regression tests

Native C regression tests that observe **the same hand-assembled bytes** through
both public entry points — `NdDecodeEx` (decoder) and `ShemuEmulate` (emulator)
— and check that the decoded register/size/high-byte/memory information agrees
with the emulated architectural state.

## What these tests do and do not do

They do **not**:

- execute the input bytes on the real CPU (the bytes are only decoded/emulated),
- depend on Windows, a debugger, a real process, or real process memory,
- use the Python test framework under `tests/`.

They do:

- run entirely in portable C11 on the host,
- supply every external memory access through one controlled `AccessMemory`
  callback that records address, width, read/write direction and order,
- derive every expected value directly from the byte encoding and the x86
  architecture rules below — a second decoder call is never used to produce an
  expected value,
- assert on structured fields (registers, `INSTRUX` operands, callback records,
  status codes) rather than diffing whole text-disassembly logs.

## Running

```sh
bash tests/native_width/run.sh
```

The script compiles the unmodified library, runs all scenarios, then applies
each degradation patch to a **temporary source copy**, rebuilds and reruns the
same binary. It ends with `NATIVE_WIDTH_OK`. In environments whose default temp
area is not writable, point the scratch root elsewhere:

```sh
NATIVE_WIDTH_TMP=/some/writable/dir bash tests/native_width/run.sh
```

The old `bash baseline/run.sh` is unaffected and still passes.

## Files

- `native_width_test.c` — the nine scenarios, the memory callback and the checks.
- `patches/mut_32bit_zero_extend.patch` — breaks 32-bit write zero-extension.
- `patches/mut_high8_selection.patch` — breaks AH/SPL high-byte selection.
- `patches/mut_addrsize_override.patch` — breaks 32-bit effective-address masking.
- `run.sh` — normal build + per-patch detection orchestration.

A mutation counts as detected only when the patch applies cleanly, the tree
compiles cleanly, the binary exits **normally with status 1** (a signal, i.e.
exit code ≥ 128, aborts the script), and its output contains the matching named
`BEHAVIOR_FAIL` marker. Apply failures and compile errors also abort instead of
being counted. The checked-in library sources are never modified.

## The three named behavior assertions

| Marker | Architectural rule guarded | Mutation that must trip it |
|---|---|---|
| `BEH_32BIT_ZERO_EXTEND` | In long mode a 32-bit GPR write zeroes the upper 32 bits | `mut_32bit_zero_extend` |
| `BEH_HIGH8_SELECTION` | `0xB4` without REX is AH (high byte); with REX it is SPL (low byte) | `mut_high8_selection` |
| `BEH_ADDRSIZE_OVERRIDE` | `0x67` computes the effective address at 32 bits and masks it before the segment base is added | `mut_addrsize_override` |

## Scenarios: bytes and hand-derived expectations

All snippets are decoded at `ND_CODE_64` and loaded at RIP `0x1000`. "Retired"
below means instructions that actually executed; note the emulator increments
its loop counter once more at the next iteration, so a clean run that retires
`N` instructions reports `InstructionsCount == N + 1`.

### 1. `width_32bit_clears_high` — 32-bit write clears the high half

```
48 B8 88 77 66 55 44 33 22 11   mov rax, 0x1122334455667788
B8 DD CC BB AA                   mov eax, 0xAABBCCDD
```

Rule: writing EAX replaces the low dword and forces the high dword to zero.
Final `RAX = 0x00000000AABBCCDD`. The decoder must report an 8-byte RAX operand
for the first MOV and a 4-byte EAX operand (no REX) for the second.

### 2. `width_16bit_8bit_preserve` — 16-bit and low-8 writes preserve the rest

```
48 B8 88 77 66 55 44 33 22 11   mov rax, 0x1122334455667788
66 B8 34 12                      mov ax, 0x1234
B0 EE                            mov al, 0xEE
```

Rule: partial writes merge into the destination instead of zeroing it.
After `mov ax`: `0x1122334455661234`; after `mov al`: `0x11223344556612EE`.
The decoder must mark the 0x66 prefix and the 2-byte AX / 1-byte AL operands.

### 3. `high8_ah_vs_spl` — AH vs SPL must not be confused

```
B4 99        mov ah, 0x99
40 B4 77     mov spl, 0x77
```

Both use opcode `0xB4` with low register id 4; only REX distinguishes them.
Without REX the decoder must return `IsHigh8=1` (AH, byte 1); with REX 0x40 it
must return `IsHigh8=0` (SPL, byte 0). Starting from
`RAX=0xAABBCCDD11223344`, `RSP=0x5566778899AABBCC`, the results are
`RAX=0xAABBCCDD11229944` and `RSP=0x5566778899AABB77` — every other bit kept.

### 4. `adc_sbb_carry_paths` — carry in/out and borrow

```
83 D0 01     adc eax, 1
83 DB 00     sbb ebx, 0
F8 / F9      clc / stc   (used in the isolated SBB runs)
```

Inputs use a high sentinel (`RAX=0xFFFF…FFFF`, `RBX=0x7777…7777`) and a
cross-byte carry:

- CF=0 on entry: `0xFFFFFFFF + 1 + 0` wraps to `RAX=0` and sets CF; the
  following `SBB EBX,0` borrows, giving `RBX=0x77777776`.
- CF=1 on entry: the extra carry gives `RAX=1` (no wrap), ZF clear.
- `CLC; SBB EBX,0` leaves `RBX=0x77777777`; `STC; SBB EBX,0` gives
  `RBX=0x77777776`.

The decoder must report CF in `FlagsAccess.Modified` for both ADC and SBB.

### 5. `cmp_cond_branch` — CMP flags drive the conditional jump

```
off 0  39 D1              cmp ecx, edx
off 2  74 07              je  +7            (target offset 11)
off 4  B8 01 00 00 00     mov eax, 1        ; not-taken path
off 9  EB 02              jmp +2            (merge past the stream)
off 11 66 90              nop               ; 2-byte NOP, taken target
```

- `ECX == EDX`: ZF set, JE taken, the `MOV EAX,1` is skipped; RAX keeps its
  sentinel `0xDEADBEEFCAFEBABE`; 3 instructions retire.
- `ECX != EDX`: JE not taken, `MOV EAX,1` runs and zero-extends, so `RAX=1`;
  4 instructions retire.

Both paths finish at RIP `0x100D` (13-byte stream). The different final RAX is
the observable consequence of a flag/branch error.

### 6. `addrsize_override_seg` — 0x67 truncation plus FS/GS bases

```
67 64 8B 87 00 20 00 00   mov eax, fs:[edi + 0x2000]
67 65 8B 0E               mov ecx, gs:[esi]
67 64 83 06 01            add dword fs:[esi], 1
```

Inputs: `RDI=0x11112222FFFFF000`, `RSI=0x5555666677778804`,
FS base `0x10000`, GS base `0x200000`.

The first effective address overflows 32 bits before masking:
`0xFFFFF000 + 0x2000 = 0x100001000`, masked to `0x00001000`; the FS base is
added only afterwards, giving linear `0x11000`. An engine that skips the mask
would instead reach `0x100011000` — this is what the address-size mutation
changes. The four recorded accesses are, in order:

1. LOAD  `0x11000` width 4 (FS, truncated EDI+disp),
2. LOAD  `0x77978804` width 4 (GS base + truncated ESI `0x77778804`),
3. LOAD  `0x77788804` width 4 (FS base + truncated ESI, RMW read),
4. STORE `0x77788804` width 4 (RMW write-back = loaded dword + 1).

The EAX/ECX loads also exercise zero-extension of 32-bit register results.

### 7. `rmw_read_modify_write_pair` — read then write is two accesses

```
FF 00        inc dword [rax]      ; RAX = 0x500026
```

The callback serves `0x1DBB69FF` at that address, so the increment performs a
real cross-byte carry to `0x1DBB6A00`. Exactly two external accesses must be
recorded — same address, same 4-byte width, load first (seq 0), store second
(seq 1) — and the stored payload must equal the loaded dword plus one.

### 8. `rejected_access_reports_reason` — clean rejection, not a crash

```
8B 07                  mov eax, [rdi]      ; RDI = 0x600000
B8 78 56 34 12         mov eax, 0x12345678
```

With the first callback forced to return false, emulation must return
`SHEMU_ABORT_GLA_OUTSIDE`, record only that single rejected 4-byte load, leave
the RAX sentinel untouched and never run the second MOV. RIP has already been
advanced past the 2-byte faulting instruction (to `0x1002`), but no register
write commits. A control run with the callback accepting succeeds and reaches
`RAX=0x12345678`.

### 9. `rerun_context_independence` — no shared mutable buffers

The scenario-6 bytes are run through two separately allocated contexts whose
stack and internal buffers are pre-filled with distinct garbage (`0x11` vs
`0x22`/`0x33`/`0x44`). GPRs, flags, RIP, counters, status, every callback
record and the (untouched) stack contents must match between the two runs, and
the shellcode bytes must be unchanged.

## Callback data invariants

- Reads are zero-filled first, then overwritten with a deterministic
  address-derived dword/qword, so tests see both genuine zero values and
  non-zero high-bit sentinels.
- The RMW address in scenario 7 is chosen so the served dword ends in `0xFF`,
  forcing an observable cross-byte carry.
- Stores keep their payload in the callback record so the read-modify-write
  value can be checked directly.

## Coverage gaps (deliberately out of scope)

These tests target the width/flag/addressing regressions described above and do
not claim to cover:

- 16-bit or 32-bit decode modes (every instruction is decoded in long mode;
  only address-size, not mode, is switched via 0x67),
- VEX/EVEX/APX/REX2 encodings, XOP and AVX/AVX-512/MMX/x87 register state,
- privileged instructions, segment-register loads, far branches, SYSCALL/INT,
- string/IO instructions, shadow-stack and CET features,
- the shellcode-detection heuristics (NOP sleds, TIB/PEB, stack strings,
  stack pivot, self-write), the direct-mapped shell option and ring 0,
- self-modifying code and instruction-cache invalidation,
- multi-byte (`> 8`) memory operands and misalignment behavior.

New behavior in those areas should get its own named scenarios rather than be
folded into this set.
