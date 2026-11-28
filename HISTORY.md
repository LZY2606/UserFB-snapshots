# Reversible debugging (instruction history)

libx86emu can run as a reversible, single-stepping debugger for small guest
firmware fragments. Instead of restarting the whole guest after an illegal
memory access, you step forward one instruction, step back to the previous
instruction boundary, change a general purpose register if needed, and step
forward again along a new branch.

The reversible engine drives the *existing* instruction executor and operation
tables. It never runs real machine code and never performs real port I/O. Both
16-bit and 32-bit code and address sizes are supported. This is a library
feature; the demo program is not turned into a debugger product.

## Public API

Declared in `include/x86emu.h`:

| Function | Purpose |
| --- | --- |
| `x86emu_history_enable(emu)` | Enable the mode on a clean instruction boundary and start with an empty history. Returns 0 if a custom `memio` or code handler is installed. |
| `x86emu_history_disable(emu)` | Leave the mode and free every recorded change. |
| `x86emu_history_step(emu)` | Execute exactly one instruction and record it. |
| `x86emu_history_back(emu)` | Undo the most recent recorded instruction. |
| `x86emu_history_get_stats(emu, &stats)` | Resource counters for the retained history. |
| `x86emu_history_len(emu)` | Number of recorded steps currently held. |
| `x86emu_history_peek(emu, addr, len, data, attr)` | Non-disturbing observer for guest memory. |

### Step result (`x86emu_history_step`)

| Result | Meaning |
| --- | --- |
| `X86EMU_HIST_OK` | The instruction completed normally and was recorded. |
| `X86EMU_HIST_HALTED` | The instruction was `HLT`. The CPU is halted; the step is still recorded and can be undone. |
| `X86EMU_HIST_MEMFAULT` | A memory permission check failed. The whole instruction was rolled back and nothing was published. |
| `X86EMU_HIST_BARRIER` | A host-visible instruction was refused *before* any handler ran. CPU and memory are unchanged and nothing was recorded. |
| `X86EMU_HIST_ERROR` | History mode is not enabled. |

### Back result (`x86emu_history_back`)

`X86EMU_HIST_BACK_OK` undoes one step; `X86EMU_HIST_BACK_EMPTY` is reported
explicitly when no step can be undone and the state is left untouched;
`X86EMU_HIST_BACK_ERROR` means the mode is disabled.

A step is one full instruction including all of its prefixes. A repeated string
operation (`REP`/`REPE`/`REPNE` `MOVS`/`CMPS`/`STOS`/`LODS`/`INS`/`OUTS`) runs to
completion inside its normal handler and counts as a single step, so `back`
undoes the entire repeated operation at once.

## What is recorded

The engine stores **change records**, never a clone of the whole emulator and
never a copy of a whole address-space page.

For every step it keeps:

1. A single fixed-size CPU register pre-image (`x86emu_regs_t`): general and
   special registers, flags, segment registers and caches, control/debug
   registers, instruction mode, and the pending-interrupt fields.
2. One byte record per *distinct* guest byte actually touched, holding its
   linear address together with the previous **data byte and the previous
   attribute byte**. Reads and instruction fetches change access attributes
   (`ACC_R`/`ACC_X`/`ACC_INVALID`/...), so those are recorded too; undoing a
   location restores both data and attributes, not just the data.
3. Page materialization events. A sparse 4 KiB page that only came into
   existence because of the step is dematerialized again when the step is
   undone.

Only locations that really change consume records. Mapping an additional
1 MiB that is never accessed does not grow the retained data/attribute
pre-images (verified by the `peek_sparse_stats` contract case).

### Aliases and repeated writes

Only the first touch of a linear address within a step keeps a pre-image.
Therefore:

* two guest pages mapped onto the same host storage, and
* a `REP` loop that rewrites one storage location several times,

both undo to the true pre-instruction content. Records are restored in reverse
order while the first-touch rule guarantees every address returns to its
original value.

### External mappings

Memory supplied through `x86emu_set_page` is owned by the caller. Such pages are
never freed or replaced by a library-allocated copy on undo; their changed bytes
are restored **in place** through byte records. Only pages the library itself
allocated lazily are dematerialized.

## What can be undone

Replayable state covers the integer CPU and the guest memory image, including
access permission/access-bit attributes and materialized pages:

* arithmetic/logic/shift instructions and the flags they set,
* conditional and unconditional branches and loops,
* near `CALL`/`RET` and the associated stack reads/writes,
* `PUSH`/`POP` and other stack accesses,
* `MOVS`/`CMPS`/`STOS`/`LODS`, including whole `REP` iterations.

After `OK` or `HLT`, `back` restores the registers, memory, attributes and the
stopped (halted) state. Stepping forward again reproduces the identical state.

Floating point, MMX and SSE/SIMD instructions are out of scope and are not
supported as reversible steps.

### Branching after going back

Going back leaves the machine on an instruction boundary. Editing a general
purpose register there and stepping forward starts a **new branch**: the
previously recorded "future" is discarded and can no longer be used to restore
state.

## Memory permission failures are transactions

A failed permission check is handled as a debug transaction. The failure is
reported as `X86EMU_HIST_MEMFAULT`, every change staged by that instruction is
reverted, and no half-instruction is appended to the history. Earlier,
successful steps remain fully undoable.

In particular, when a `REP` string op writes several bytes and then reaches a
non-writable location, the rollback restores:

* the string registers (`CX/ECX`, `SI/ESI`, `DI/EDI`),
* every byte already written,
* the `VALID`/access bits and the `invalid` marker.

This is a convention of the debug entry only; the error semantics of the normal
`x86emu_run` loop are unchanged.

## Side-effect barriers

Instructions that may observe or alter the host cannot be reconstructed from
memory/register change records, so they return `X86EMU_HIST_BARRIER` **before
any processor callback or host access happens**, with the CPU and memory kept at
their pre-step state:

* port I/O: every `IN`/`OUT` form (immediate port and `DX`),
* software interrupts (`INT3`, `INT imm8`, `INTO`) and any fault that would be
  delivered to an installed external interrupt handler,
* `CPUID`, `RDMSR`, `WRMSR`,
* `RDTSC` (time stamp reads).

No host side effect is produced first and "pretended" to be withdrawn. A
counting callback that performs no I/O can be used to prove the handler is never
invoked (see the `barriers` contract group).

## Non-disturbing observation: `x86emu_history_peek`

`x86emu_history_peek` copies data and/or attribute bytes into caller buffers.
It:

* sets no access bits and changes no attributes,
* materializes no sparse page,
* returns `0xff` data and the memory default attribute for bytes of a page that
  has not been materialized.

Use it to inspect state while debugging; do not use the public
`x86emu_read_*`/`x86emu_write_*` accessors for that purpose during a session.

## Actions that invalidate the history

While the mode is enabled, any guest change the change recorder cannot follow
discards the retained history and releases its records:

* mapping/permissions setters: `x86emu_set_page`, `x86emu_set_perm`,
  `x86emu_set_io_perm`,
* processor/handler setters: memio, code, interrupt, CPUID, RDMSR, WRMSR,
* `x86emu_reset`,
* normal `x86emu_run`,
* the public memory accessors `x86emu_read_*` and `x86emu_write_*`,
* `x86emu_reset_access_stats`.

Enabling the mode is refused outright when a custom `memio` or code handler is
already installed, because their effects cannot be recorded or undone.

After `x86emu_history_disable`, plain `x86emu_run` and all other entry points
behave exactly as before.

## State that is intentionally not replayable

The following are deliberately excluded from the reversible state and are not
restored by `back`:

* the time stamp counter and wall-clock MSRs (`R_TSC`, `R_REAL_TSC`,
  `R_LAST_REAL_TSC`),
* the trace log buffer and its pointers,
* I/O port permission/access maps and port statistics,
* allocator identity of internal objects (the *content* of guest memory and
  registers is restored, but pointer values internal to the library are not
  part of the contract).

During a debugging session the caller must not modify guest memory or internal
pointers directly; only general purpose registers may be edited, and only on an
instruction boundary (i.e. after a step or a `back`).
