# VGM 1.71 DAC stream control

This document describes the DAC stream control (VGM commands `0x90`–`0x95`)
implemented inside the VGM player. The implementation lives in
`gme/Vgm_Stream.{h,cpp}` and is driven from the VGM command loop in
`gme/Vgm_Emu_Impl.cpp`. The public C API is unchanged.

The command layout follows the VGM 1.71 specification:
<https://vgmrips.net/wiki/VGM_Specification>

## Supported scope

Only the explicitly supported subset is interpreted; anything outside it is
read past in full, reported through `gme_warning()`, and stops the affected
stream. Its bytes are never reinterpreted as later commands.

* First YM2612 chip, port 0, register `0x2A` (the DAC register).
* Uncompressed data bank 0.
* Stream IDs `0`–`254`. ID `0xFF` is reserved and only meaningful to `0x94`.
* `0x93` start length mode 1 — length counted in sample writes — including the
  loop flag in bit 7.
* `0x95` fast start by block number, including the loop flag in bit 0.
* Frequencies from 1 to 44100 Hz.

Not supported (warned and stopped): reverse/other `0x93` length modes, the
`offset = 0xFFFFFFFF` special case, compressed or non-zero banks, other sound
chips/ports/registers, and stream ID `0xFF` anywhere except `0x94`.

## Commands

All multi-byte integers are little-endian.

| Command | Payload | Meaning |
| --- | --- | --- |
| `0x90` | `id chipType chipTarget regAddr` | DAC stream setup |
| `0x91` | `id bank step base` | Set data bank, step and base |
| `0x92` | `id frequency(32)` | Set stream frequency in Hz |
| `0x93` | `id offset(32) lengthMode length(32)` | Start a write-count range |
| `0x94` | `id` | Stop one stream; `0xFF` stops all |
| `0x95` | `id blockId flags` | Fast start over one block |

## Data bank and block model

Every type-0 `0x67` data block in the file is concatenated, in first-seen
order, into a single logical bank. Each block keeps its bank start position
and size, so bank byte offsets map to the correct physical block.

* A block becomes available when playback reaches its `0x67` command.
* The same physical block reached again through a VGM loop is neither appended
  nor numbered a second time (deduplicated by address), so the bank grows with
  the number of distinct blocks in the file, never with the loop count or
  playback duration. No per-sample allocation occurs during playback.
* For `0x93`, `offset` is a byte position in the bank; values are read starting
  at `offset + base` and advance by `step` each write. A mode-1 range may cross
  block boundaries.
* For `0x95`, values start at the named block's start plus `base`, and every
  value including the last stays inside that one block; the next block is never
  borrowed.
* A start's range is fixed when issued. Blocks appended later do not extend it.
* The legacy `0xE0` seek and `0x8n` single-byte PCM read address the same
  concatenated bank through their own cursor, independent of every stream, and
  are bounds-checked so they cannot form an out-of-bounds pointer.

## Timing

Each stream runs on the VGM 44100 tick/second clock, independent of the output
sample rate and of the size of each `gme_play()` call.

* A start writes its first value immediately at the start tick, then keeps an
  integer phase accumulator. Every tick adds `frequency`; when the accumulator
  reaches 44100 the next value is written and 44100 is subtracted. With a
  constant frequency, write `k` (from 0) therefore lands at
  `start + ceil(k * 44100 / frequency)`.
* Long waits that span several internal render frames still place each value at
  its exact due tick; values are never batched at the end of a wait.
* Ordering at a tick `T`: stream writes already due at `T` run before the file
  command at `T`; several streams due together run in ascending stream ID; file
  commands on the same tick keep file order.
* A value whose tick equals an internal frame boundary is attributed to the
  next frame, matching how an explicit register write at that tick is deferred
  by the command loop. This is what makes a stream-controlled file
  sample-identical to a fully expanded sequence of direct `0x52` writes.
* Tempo changes speed the streams and the direct file register writes together.

## State lifecycle

A stream ID moves through these states:

1. **Unconfigured.** No accepted `0x90` has been seen. Any `0x91`–`0x95`
   targeting it warns and stops (or is ignored for the reserved ID).
2. **Configured.** A supported `0x90` was accepted. `0x91` sets bank/step/base,
   `0x92` the frequency. Re-issuing `0x90` or `0x91` first stops the stream and
   resets its playback state.
3. **Running.** A `0x93`/`0x95` start replaces any previous range and clears
   the phase, then writes the first value immediately.
   * `0x92` while running keeps the current position and the phase remainder;
     the new frequency applies from that tick and no extra first value is
     written.
   * When the write count reaches zero, a looping stream returns to the start
     of its pass (the next value still waits for the next sample period); a
     non-looping stream stops.
4. **Stopped.** After `0x94`, a natural end, or a reconfiguration. No further
   values are written; a later start emits a fresh first value immediately.

`0x94` with ID `0xFF` stops every stream. Elsewhere `0xFF` is ignored with a
warning.

A VGM file loop only moves the command pointer. Streams that have not been
stopped keep running on their own phase across the jump; they are not rebuilt
on every loop. A backwards seek, a new `start_track`, and loading another file
into the same emulator all reset the controller, dropping every stream, the
bank and the legacy cursor.

## Error handling

Two classes of malformed input are distinguished.

* **Truncated command / block** — a new `0x90`–`0x95` command missing
  parameters, or a `0x67` block whose length runs past the file end: a warning
  is raised and playback ends in a controlled way (the command cursor is moved
  to the file end). No pointer past the buffer is formed.
* **Invalid stream parameter** — an unconfigured stream, invalid block number,
  out-of-range offset, `step == 0`, `base >= step`, or a zero/out-of-range
  frequency: a warning is raised and only that stream is stopped. Other valid
  streams are unaffected.

All position and count arithmetic checks for overflow before use.

## Verification

The test program is `test/vgm_stream_control.cpp`. It builds each scenario
twice: once using the stream commands, and once as an independently computed
reference that works out event ticks (integer phase) and bank byte values
itself and expands them into plain `0x52` writes and waits. Both are rendered
through the real audio chain and compared sample for sample; the reference
never calls the controller and never infers events from emulator output.

Run the whole verification (fresh library build, six cases, the direct-write
smoke test and the NSF file/memory examples):

```sh
bash support/target.sh
```

It ends with `VGM_STREAM_CONTROL_OK cases=6`. A single case can be run with:

```sh
test/vgm_stream_control --case bank_blocks
```

The six cases are `bank_blocks`, `rational_waits`, `controls`, `loop_restart`,
`invalid` and `legacy`. An unknown case name exits non-zero. Each passing case
prints `VGM_STREAM_CASE <name> PASS`.
