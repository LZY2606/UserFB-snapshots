# Adaptive-stage contract tests

`tests/adaptive_contract.c` implements `task_group()` from
`task_support/check.h` with three groups — `dither`, `predictor` and
`stream` — each reporting individual comparisons through `TASK_CHECK` and
returning 1 on success. Build and run with:

```sh
bash task_support/build.sh          # cc -std=c99 -O2, system compiler only
./.task-build/target dither
./.task-build/target predictor
./.task-build/target stream
```

`bash task_support/target.sh` runs all three groups against the unmodified
library and then against two mutated source trees (a codeword history that
shifts one bit too little, and a predictor dot product that reads the
previous tap). Each mutation must still compile and must be reported by a
content comparison as `CONTENT_MISMATCH` with exit code 42; a build
failure or a crash does not count as detection.

## Independence of the reference model

The expected values come from a reference model that is maintained
independently from the production algorithm:

- **Own state.** `struct ref_channel` / `struct ref_subband` are zero-initialized
  and advanced inside the test file. Nothing is ever copied out of
  `struct aptx_context`; production state is only read for comparison.
- **Own arithmetic.** Rounding shifts are expressed as wide-integer
  quotient/remainder (`ref_floor_div`, `ref_mod`) with an explicit tie
  correction, saturation as explicit range clamps, and 32-bit truncation as
  explicit two's complement (`ref_trunc32`, `ref_s32`). The production
  rounding, clipping, dither, invert-quantize and prediction helpers are
  never called to compute an expectation.
- **Own history layout.** The residual history is a plain newest-first
  array shifted with `memmove` (keeping `order + 1` residuals, which is
  what the production mirror-ring actually exposes), not a copy of the
  production ring buffer.
- **Own bit packing.** Codewords are packed and unpacked per an explicit
  field plan (`ref_plan`): aptX uses a 16-bit word with 7/4/2/3-bit fields,
  aptX HD a 24-bit word with 9/6/4/5-bit fields; the highest subband field
  drops its least significant bit on pack and recovers it from the parity
  of the remaining bits on unpack.
- **Tables only.** The format constant tables (intervals, dither factors,
  factor-select offsets, quantization factors) are read from the production
  source, which the test includes to observe the static stages. The
  production sources and the public API are unchanged: a normal build gains
  no callback, state or allocation.
- **Wire-driven stream expectations.** The stream group never re-encodes
  to obtain an expectation. The model is driven by codewords parsed out of
  the bytes actually emitted by `aptx_encode`/`aptx_encode_finish`,
  unpacked with the independent field plan.

## Covered state

### dither group

- 32-bit codeword history, four subband dither values and dither parity,
  derived per beat from an independently maintained sequence of previous
  codewords (positive, negative, zero and field-extreme values).
- Left and right channels run different histories, seeded with high bits
  set so the 32-bit history wraps while the sequence runs (40 beats).
- Pack/unpack round trips for both aptX and aptX HD field widths, including
  out-of-range values that exercise the field masks, and parity recovery of
  the highest subband's least significant bit.
- Sixteen consecutive beats of the sync parity cycle (two full 8-beat
  turns), checking both the parity verdict and the sync index.

### predictor group

For every subband of both formats, beat by beat over sequences of
`2 * order + 48` beats (longer than each prediction order 24/12/6/12, so
the residual history genuinely wraps):

- the quantization factor used for the current symbol (checked before the
  update, proving the current symbol uses the old factor and the updated
  factor is left for the next symbol),
- inverse-quantized value (`reconstructed_difference`), `factor_select`,
  next-beat quantization factor,
- both pole weights (`s_weight`), all zero weights (`d_weight`),
  previous signs,
- reconstructed sample, predicted difference and predicted sample.

Inputs alternate signs, include zeros, stable small values followed by a
sudden increase, near-limit codewords and pseudo-random values, with dither
extremes interleaved. Rounding stages and the 24-bit saturations are
checked separately, not merged.

### stream group

- Public `aptx_encode`/`aptx_decode` on sustained stereo 24-bit audio
  (ramps, alternating signs, noise, silence, near-clip, small-then-sudden
  segments), for aptX and aptX HD.
- Two instances (one per format) run interleaved; each keeps its own model
  history.
- After every emitted codeword, the encoder's and the decoder's internal
  predictor state (history, dither, quantized samples, factors, weights,
  reconstructed and predicted values) are compared against the model, so
  encoder and decoder predictor states are proven consistent with each
  other.
- Public consumed/`written` counts on every call; the 90-sample latency
  (silent first 22 codewords, 2 samples on the 23rd, 4 per codeword after)
  is checked per call and in total.
- `aptx_encode_finish` with deliberately short output buffers, verifying
  partial writes, continuation calls, the final reset of the encoder, and
  decoding of the 23-codeword latency tail one codeword at a time.
- Reuse after `aptx_reset` with a re-initialized model.
- Decoded PCM is lossy and is not compared against the input; the QMF stage
  has no separate reference.

## First difference

Every comparison is an individual `TASK_CHECK`. On the first divergence the
harness prints `CONTENT_MISMATCH file:line <field> got=<a> want=<b> at
(<indices>)` — naming the diverging field, both values and the beat /
subband / channel coordinates — and exits with code 42, so the report
pinpoints the first divergent beat rather than a final hash. Groups run in
the order `dither`, `predictor`, `stream` and stop at the first failing
group. Observed first differences for the shipped mutations:

- `history.patch` (history shifted one bit short): `codeword history` in
  the dither group, beat 0.
- `lag.patch` (dot product reads the previous tap): `predicted difference`
  in the predictor group, as soon as the residual history holds two
  different values.
