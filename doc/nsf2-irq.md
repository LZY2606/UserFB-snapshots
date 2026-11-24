# NSF2 IRQ support

Reference specification: <https://www.nesdev.org/wiki/NSF2>

This document describes the NSF2 subset implemented in Game_Music_Emu's NSF
emulator (`gme/Nsf_Emu.*`, `gme/Nes_Cpu.*`, `gme/nes_cpu_io.h`).

## Supported scope

Only the following NSF2 files are accepted:

- header version byte (`$0005`) set to `2`;
- no expansion sound chips (`$007B == 0`);
- the 24-bit program-length field at `$007D..$007F` is zero (so the program
  occupies the remainder of the file and there is no NSFe suffix);
- the NSF2 feature byte at `$007C` only uses:
  - bit `4` (`$10`) — NSF2 IRQ timer and writable IRQ vector;
  - bit `6` (`$40`) — PLAY subroutine is disabled.

Anything else is rejected by `gme_open_data()`/`gme_open_file()` with a clear
`gme_err_t` error:

- feature bit `5` (`$20`, non-returning INIT / NMI-driven PLAY);
- feature bit `7` (`$80`, mandatory NSFe metadata);
- reserved feature bits `0..3`;
- a non-zero program-length field;
- NSF2 expansion sound hardware.

Version 1 NSF files ignore byte `$007C` (and `$007D..$007F`) completely, so
all existing NSF files, the plain C interface and NSFe behaviour are
unchanged.

## IRQ timer (`$401B..$401D`)

When feature bit `4` is set:

- `$401B` / `$401C` are the low/high bytes of a readable and writable 16-bit
  reload value.
- Writing `$401D` bit `0` starts (`1`) or stops (`0`) the timer.
- Reading `$401D` returns:
  - bit `7` — timer IRQ flag, reported **before** it is cleared;
  - bit `0` — current running state.

  The read acknowledges only the timer source. Frame-counter and DMC IRQ
  requests are not acknowledged by it.
- The countdown period is `reload + 1` CPU cycles.
- Changing the reload while the timer is running does not move the current
  countdown deadline; the new reload applies to the next period.
- While stopped the countdown follows the reload value; a pending
  (unacknowledged) IRQ flag survives stop/start and reload writes.

Frame-counter IRQs, DMC IRQs and the NSF2 timer are level-ORed into the CPU
IRQ line. Reading `$401D` therefore never accidentally clears an APU request,
and reading `$4015` never clears the timer flag.

## IRQ vector override (`$FFFE/$FFFF`)

When feature bit `4` is set, `$FFFE/$FFFF` form an independent writable IRQ
vector override:

- at track start, before INIT, the override is loaded from the IRQ vector in
  the initially mapped ROM banks;
- writes to `$FFFE/$FFFF` update the override;
- ordinary CPU reads, optimized program reads and the CPU's interrupt-vector
  fetch all see the same two bytes (the top `$F800..$FFFF` code page is backed
  by a shadow page);
- writing `$5FFF` to swap the `$F000..$FFFF` bank refreshes the other bytes
  from the newly selected bank while preserving the vector override;
- the original loaded input/ROM buffer is never modified.

## CPU integration

Timing uses the existing bus-callback time base (`Nes_Cpu::time()`); the CPU
core was not converted to per-bus-cycle emulation. IRQ requests are sampled at
instruction boundaries through the existing `CPU_DONE` hook, honouring the I
flag (`SEI`/`CLI`/`PLP`/`RTI`). Interrupt entry performs the real 6502 stack
pushes and vector fetch, and `RTI` restores the interrupted context. The
status byte pushed for an IRQ has the B bit (`$10`) clear and the unused bit
`$20` set; BRK still pushes B set.

After a track starts the reset state has I set and the timer off. Once INIT
returns (typically after `CLI`), the idle trap at `$5FF8` is fast-forwarded
only to the next timer/APU event, so idle waiting is woken by an IRQ.

Feature bit `6` only suppresses periodic PLAY injection. Normal PLAY calls and
NSF2 IRQs can be active together: an IRQ during PLAY returns through `RTI` to
the PLAY routine, whose `RTS` then returns to the saved execution context.
PLAY is never used as the IRQ vector and sound registers are not reset per
chunk.

Timer deadlines and APU IRQ deadlines are rebased together with
`run_clocks()`' time origin at every output chunk, so rendering a whole buffer
or alternating `1/7/257` stereo-frame reads (`2/14/514` samples) produces
identical PCM. Starting a track, reloading a file and seeking backwards reset
the timer, pending flags and vector override from the new/initial ROM state.

## Tests

`test/nsf2_irq_task.cpp` registers `timer`, `cpu` and `audio` cases with
`task_support/check.h` (no `main`; driven by `task_support/driver.c`). Run:

```sh
bash task_support/run.sh target
```
