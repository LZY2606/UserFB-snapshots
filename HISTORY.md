# Reversible debugging (history mode)

libx86emu can record instruction executions as undoable steps, so a
debugging session over a local firmware fragment can go forward one
instruction and then back again without re-running from the start.

The interface is declared in `include/x86emu.h`:

```c
int      x86emu_history_enable(x86emu_t *emu);
void     x86emu_history_disable(x86emu_t *emu);
unsigned x86emu_history_step(x86emu_t *emu);
unsigned x86emu_history_back(x86emu_t *emu);
void     x86emu_history_stats(x86emu_t *emu, x86emu_history_stats_t *stats);
void     x86emu_history_peek(x86emu_t *emu, u32 addr, unsigned len,
                             u8 *data, u8 *attr);
```

## Enabling

`x86emu_history_enable()` must be called at a clean instruction boundary
and starts an empty history. It refuses (returns non-zero) if a custom
memio handler or a code handler is installed, because those would make
single steps unrecoverable. `x86emu_history_disable()` releases all
recorded state; afterwards plain `x86emu_run()` behaves exactly as
before.

## Stepping

`x86emu_history_step()` executes exactly one instruction, including all
prefixes; a REP string instruction runs to completion and counts as a
single step. It returns:

* `X86EMU_HISTORY_OK` – the instruction completed and was recorded.
* `X86EMU_HISTORY_HLT` – the instruction halted the cpu (also recorded;
  HLT can be undone). Stepping an already halted cpu is a no-op that
  returns `X86EMU_HISTORY_HLT` again.
* `X86EMU_HISTORY_MEMERR` – the step hit a memory permission failure
  (missing read/write/execute permission, or an instruction fetch from
  invalid memory). The step is treated as a failed debug transaction:
  every change it staged – registers, already written bytes, VALID and
  access attribute bits, the memory `invalid` flag, and pages it
  materialized – is rolled back and nothing is published to the
  history. Previously recorded steps can still be undone. This is a
  convention of the debugging entry point only; the error semantics of
  plain `x86emu_run()` are unchanged.
* `X86EMU_HISTORY_BARRIER` – the next instruction could observe or
  change the host: port I/O (`in`/`out`/`ins`/`outs`), software
  interrupts (`int`/`int3`/`into`), `cpuid`, `rdmsr`/`wrmsr`, or
  timestamp reads (`rdtsc`/`rdpmc`). The step is *not* executed at all:
  cpu and memory stay exactly as they were and no handler is called.
  A pending interrupt with an external intr handler installed, or a
  fault that would call it, also stops at the barrier.
* `X86EMU_HISTORY_DISABLED` – history mode is not enabled.

`x86emu_history_back()` undoes the most recent recorded step and returns
`X86EMU_HISTORY_BACK_OK`, or `X86EMU_HISTORY_BACK_EMPTY` when there is
nothing (more) to undo.

After a successful or HLT step, going back and then forward again
reproduces identical registers, memory and stop state. If you go back,
modify a general register (the only modification allowed at an
instruction boundary while debugging), and then step, a new branch is
created: the old future is discarded and can no longer be restored.

## What is recorded and restored

History entries store *change records*, not emulator clones:

* a fixed-size CPU state pre-image (the `x86emu_regs_t` register block
  and the emulated time stamp counter);
* for memory, only the old values of the bytes the step actually
  touched – both data bytes and the access/permission attribute bytes.
  Reads and instruction fetches also change attributes (accessed bits),
  and those are restored too, not just data;
* pages that were materialized (allocated) by the step become
  unmaterialized sparse pages again; pages mapped by the caller via
  `x86emu_set_page()` are restored *in place* in the caller's storage,
  never replaced by a library-allocated copy;
* change records are keyed by host storage address, so two guest pages
  aliasing the same host memory, or a REP instruction rewriting the same
  storage many times, still restore the true pre-instruction content
  when restored in reverse order;
* rollback writes bytes back directly, never through the normal write
  interface, so undoing does not create new write-access records.

Because pre-images are sparse, mapping e.g. an extra 1 MiB of memory
that is never accessed does not grow the recorded pre-image of a two
byte write. `x86emu_history_stats()` reports the number of retained
history entries and the retained data/attribute pre-image bytes.

`x86emu_history_peek()` copies data bytes and attribute bytes into
caller buffers without setting access bits and without materializing
sparse pages; unallocated bytes read as `0xff` with the default
attributes. Use it – not the normal read functions – to observe state
while debugging.

## What invalidates the history

While history mode is active, the following operations discard the
recorded history and release its records (history mode is left
disabled):

* changing mappings, permissions or handlers through the official
  setters (`x86emu_set_perm`, `x86emu_set_page`, `x86emu_set_io_perm`,
  `x86emu_set_*_handler`);
* `x86emu_reset()` and plain `x86emu_run()`;
* the public memory read/write functions (`x86emu_read_*`,
  `x86emu_write_*`) and `x86emu_reset_access_stats()`.

During a debugging session the caller must not write memory or internal
pointers directly; only general registers may be modified, and only at
instruction boundaries.

## Scope and limitations

* 16 bit and 32 bit modes are supported for the integer instruction set:
  arithmetic, branches, near call/ret, stack accesses and the string
  instructions MOVS/CMPS/STOS (and friends) can be stepped forwards and
  backwards. Floating point and SIMD instructions are not supported.
* Instructions with possible host side effects stop at the barrier (see
  above) instead of being emulated.
* Log output and wall-clock time (the "real" tsc MSRs) are not part of
  the replayable state.
* History mode is per emulator instance and not thread-safe; do not
  drive the same emulator from multiple threads.
