# Float wire bit-level regression

WavPack 32-bit floating-point audio cannot be checked by numeric comparison
alone: negative zero, NaN payloads and tiny denormals are defined by their
exact 32-bit IEEE-754 pattern, not by a floating-point value. This directory
adds an independent, bit-level compatibility regression for the float
sideband (the `wvx` extension stream and the `FLOAT_INFO` metadata) that
pins down the on-disk contract in addition to the existing whole-file
round trips.

## How to run

```sh
bash task_support/run.sh baseline   # existing 16-pattern float round trip
bash task_support/run.sh target     # this regression plus two semantic mutants
```

`target` first builds and runs the normal regression. It then rebuilds the
program against two deliberately mutated copies of `src/unpack_floats.c`:

1. `negative_zero` — the negative-zero sign bit is consumed from the
   sideband but discarded;
2. `nan` — the NaN mantissa recovered from the sideband is cleared.

Each mutant must print a `FAIL` line and exit with status 42. A build
failure, a crash, or a run with zero assertions does not count as catching
the change.

## File layout

- `tests/float_wire_model.c/.h` — the independent reference. It accepts only
  IEEE-754 bit patterns in `uint32_t` words and its own LSB-first bit array.
  It never includes `WavpackStream`, never performs floating-point
  arithmetic, and never calls the production `getbits`/`putbits` macros or
  float conversion helpers to compute an expectation. Format constants are
  expressed as documented literal values.
- `tests/float_wire_packets.c/.h` — public-container material: a hand-written
  little-endian WavPack block/metadata chunk walker, in-memory public-API
  reader glue, real encode/decode through `WavpackOpenFileOutput()`,
  `WavpackPackSamples()` and `WavpackOpenFileInputEx64()`, and the
  hand-assembled new-format WVX material. The same model serves both
  directions.
- `tests/float_wire_test.c` — `void task_cases(void)` driven entirely by
  `TASK_CHECK` (no `main`; `task_support/driver.c` provides it). This is the
  only file that includes `wavpack_local.h`, because it calls the real
  `scan_float_data()`, `send_float_data()` and `float_values()`.

## What the independent reference proves

### Encoding scan (`scan_float_data`)

For every catalog block the reference independently derives, from explicit
sign/exponent/mantissa fields:

- the shared block exponent (`float_max_exp`), including the
  `max_mag + 0x7f0000` rounding rule;
- each signed main integer after alignment and any `float_shift` reduction;
- the `float_flags` selection, covering all four shifted-bit paths
  (`FLOAT_SHIFT_ONES`, `FLOAT_SHIFT_SAME`, `FLOAT_SHIFT_SENT`, and the
  no-WVX integer-only path), plus `FLOAT_ZEROS_SENT`,
  `FLOAT_NEG_ZEROS` and `FLOAT_EXCEPTIONS`;
- `crc_x`, rolled only over the raw bit patterns;
- the integer magnitude steps that program the header `MAG` field.

A mismatch reports the first sample whose main integer differs, or the
first flag/CRC field that differs, instead of a generic round-trip failure.

### Sideband emission (`send_float_data`)

The reference plans the exact LSB-first sideband bit sequence for the whole
block — one shared stream, consumed value after value with no per-sample
realignment — including NaN markers and 23-bit payloads, infinity markers,
false-zero fields, negative-zero sign bits and every shifted-bit scheme.
The production `Bitstream` is captured through its own fields (completed
cells plus the pending shift register), and the first diverging **bit
position** is reported. Trailing padding is never counted as valid bits.

### Sideband restoration (`float_values`)

Self-assembled main integers and sidebands are fed to the real
`float_values()`, while the reference restores every pattern independently
from the same shared bit array. Both directions compare the full 32-bit
pattern (sign, exponent and mantissa) and the independently rolled `crc_x`,
and the reference verifies that the bits it consumes per value account for
the whole sideband exactly.

The new WVX bitstream format (`ID_WVX_NEW_BITSTREAM`) is covered with
hand-built material because the current encoder never emits it. Two blocks
exercise the `min_shifted_zeros` / `max_shifted_ones` elision: omitted low
zeros and high ones must be reconstructed, the next sample must start at
the correct bit position, and physical padding bytes must stay outside the
valid bit count. Padding is sized generously for the production
`Bitstream` read convention; it does not change corrupt-stream handling.

### Public API behavior

Stereo (2-channel) and 5.1 (6-channel) material with different per-channel
bit-pattern plans is encoded through the public output API across several
16-frame blocks and decoded in small batches through the public reading
interface, without `OPEN_NORMALIZE` (the default non-normalizing read
behavior is preserved). Every returned `uint32_t` pattern, the frame count
and the cumulative error count are compared. The chunk walker records which
`FLOAT_INFO`/WVX flags actually appear in each parsed block — observations
are tied to specific metadata chunks, never to "the file contains this
byte". A pure integer-precision file is additionally asserted to carry no
WVX chunk at all. Hybrid lossy and DSD are not exercised.

The catalog contains ordinary signed audio, integer-precision floats, an
all-zero block, mixed positive/negative zeros, the smallest and largest
denormals, finite values small enough that their main integer becomes a
false zero, and positive/negative infinities plus NaNs of both signs with
distinct mantissas. A large finite anchor in each block forces the shared
exponent to change the representation of the other samples.

## What the existing MD5 round trip proves

`wvtest` (see `cli/wvtest.c`) generates audio, encodes it, decodes it, and
compares an MD5 of the decoded bytes against the MD5 of the source (and
checks the stored MD5 metadata) across speed modes, channel counts and
block layouts. That answers a different question: it proves end-to-end,
whole-file byte equality of the current encoder/decoder pair over broad
material. It cannot say *where* a disagreement starts, and it cannot detect
a coordinated wire-format change where encoder and decoder drift together
but still read each other's output back correctly. The independent
reference fills that gap by pinning each derived quantity and sideband bit
to its own calculation, so a one-sided format change fails at the first
shared exponent, main integer or sideband position that moves.

## Recorded first divergences under the semantic mutants

Normal run: `TARGET checks=267`.

- `negative_zero` mutant:

  ```
  first divergence [zeros]: production float_values sample 2 want=0x80000000 got=0x00000000
  FAIL .../tests/float_wire_test.c:287: 0
  float semantic change negative_zero detected
  ```

  The discarded sign turns negative zero (`-0.0`, pattern `0x80000000`)
  into positive zero at the first negative-zero sample.

- `nan` mutant:

  ```
  first divergence [exceptions]: production float_values sample 2 want=0x7fc00001 got=0x7f800000
  float semantic change nan detected
  ```

  Clearing the restored mantissa turns the quiet NaN payload
  (`0x7fc00001`) into positive infinity (`0x7f800000`) at the first NaN
  sample.

Both mutant probes exit 42; the wrapper only reports "detected" after
observing the `FAIL` line and that exit status.
