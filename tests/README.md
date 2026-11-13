# Entropy regression reference

This directory holds an independent C regression for the TTA1 two-level
Rice entropy coder.  Run it through the fixed runner:

```
bash devcheck/target.sh
```

The runner compiles the three groups in `entropy_reference.c` together
with `devcheck/target_main.c`, runs them, runs the existing round-trip
baseline (`devcheck/baseline.sh`), and then builds two deliberately
broken temporary copies of the library:

* `encoder_k` — codes a symbol with `k0 + 1` instead of the current
  `k0`;
* `decoder_base` — adds back the level-1 base with the `k1` shift
  instead of the current `k0` base.

Both mutants must still compile, then fail the *same* test binary with a
non-zero status and a line beginning `ENTROPY_MISMATCH ` naming the first
symbol or bit position that diverges.  A compile failure, empty output or
crash is never accepted as a detection.  Normal runs also print the
symbol counts, FIFO cut points and frame counts actually processed
(`TARGET_STATS ...` lines).

## Why a separate reference

The production encoder and decoder share one set of Rice constants and
the same adaptation rules.  A plain encode-then-decode round trip inside
that library cannot catch a mistake made identically on both sides.
The reference here is written from the format rules and does **not**
call, index or reuse any of:

* the production `tta_encoder_put_value` / `tta_decoder_get_value`
  coding loops (the test drives them, but never copies them as the
  oracle);
* the production `bit_mask` / `bit_shift` / `shift_16` tables;
* the production CRC table or CRC helpers.

Its bitstream is an independent, bit-at-a-time LSB-first reader/writer;
its CRC-32 is an independent table-free bitwise implementation.  The
32-bit shift saturation used by the format (thresholds and bases clamp
at the sign bit) is re-derived from the 32-bit arithmetic rule rather
than read from a library table.

`libtta.c` is included by the translation unit for one reason only: to
observe the internal `TTA_adapt` entropy state and to drive the internal
put/get entry points on hand-built byte streams.

## Rules encoded by the reference

* Residual to unsigned value: positive `x -> 2x-1`, non-positive
  `x -> -2x`.
* A symbol below the old level-0 base `2^k0` uses level 0; otherwise the
  base is subtracted and the remainder is expressed with level-1
  unary/remainder.
* A symbol is coded with the **pre-update** `k0`/`k1`; updated
  parameters only affect the next symbol.
* `sum0` tracks the full unsigned value.  `sum1` tracks the post-base
  quantity only when level 1 is used.  Each update first subtracts one
  sixteenth of the old sum.
* A sum exactly equal to an adaptation threshold leaves the parameter
  unchanged; only strictly crossing the lower/upper threshold moves it.

Every step is checked for bit count, the actual bit string, both
running sums and both parameters — not merely for recovering the same
residual.

## The three groups

* `case_thresholds` — three independent sequences (zeros; strictly
  sign-alternating values constructed to hit exact threshold equality
  and the adjacent crossing values; a low-amplitude decay followed by a
  sudden high-amplitude burst).  Each sequence is checked in both
  directions: the independent bitstream fed to the production decoder,
  and the production encoder output parsed independently.
* `case_fifo` — unary runs that cross the 23-bit slice boundary, 8-bit
  byte flushes and the 5120-byte FIFO cut, including a long unary
  symbol constructed to straddle a FIFO boundary, plus a tail
  valid-bit sweep checking zero padding, CRC and the exact consumed
  position under different trailing bit counts.
* `case_frames` — the public 16/24-bit, 1..6 channel encode/decode
  entries on code-generated, channel-distinct audio spanning two full
  frames plus a short tail.  The TTA header, seek table, per-frame
  lengths and per-frame CRCs are parsed independently, and the original
  PCM is compared sample by sample.  Rice state is rebuilt per channel
  per frame and is never carried over from another frame or channel.
  The 24-bit buffers keep the spare tail space the existing interface
  reads/writes; only the valid sample bytes are compared.

All material and all expected values are generated in C.  No recordings,
FFmpeg or any other codec are involved.

## What these tests do not prove

The reference targets the **entropy layer and the TTA1 container
framing** only.  In particular it does not independently validate:

* the adaptive hybrid filter's quantization/weight update behaviour
  (`hybrid_filter_enc`/`hybrid_filter_dec` in `filter.h`), including the
  SSE2/SSE4 and ARM assembly variants;
* the fixed order-1 predictor and the multi-channel (mid/side-style)
  inter-channel transform, except indirectly through the public
  sample-exact round trip in `case_frames`;
* numerical or filtering behaviour of any stage ahead of the Rice
  coder — those stages reuse the library implementation on both sides
  of the round trip, so a self-consistent filter change is outside the
  fault model this reference is designed to detect;
* the ID3v2 skip path, password/encryption handling or seek-table
  *seeking* behaviour (the seek table contents and CRC are parsed, but
  positional seeking is not exercised);
* platform-specific unaligned-access and SIMD behaviour.  A sanitizer
  build reports pre-existing unaligned `TTAint32` accesses inside the
  production 24-bit `READ_BUFFER`/`WRITE_BUFFER` macros; those are part
  of the existing library and are intentionally left unchanged.
