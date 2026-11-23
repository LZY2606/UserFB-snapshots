# Adaptive contract regression tests

`tests/adaptive_contract.c` is an independent numerical regression for the
fixed-point stages of libfreeaptx. It is compiled by
`task_support/build.sh` together with `task_support/main.c` and run by
`task_support/target.sh` as three groups:

```
.task-build/target dither
.task-build/target predictor
.task-build/target stream
```

Each group returns 1 only after every `TASK_CHECK` in it passed. A failed
comparison reports the file, line and the actual values through
`CONTENT_MISMATCH` and exits with 42. The test never defines `main`.
`task_support/main.c` supplies it and prints `DONE <group> checks=<n>` plus
`PROCESS_COMPLETE` on success.

The production algorithm and the public API are unchanged. `freeaptx.c` is
`#include`d only by this test translation unit so the stream group can read
state that is otherwise private; nothing in the library is compiled
differently, and no callback, state field or allocation is added.

## Model independence

The expected values come from a reference model that is separate from the
production code:

* Wide 64-bit intermediate arithmetic throughout.
* Floor division is computed from `a / b` and `a % b` and corrected, rather
  than relying on implementation-defined C division for negative values.
* Round-to-nearest, ties-to-even is expressed explicitly as
  `floor(v / d)` plus a `2 * remainder` comparison, not by the production
  `rshift32` / `rshift64` macros.
* 24-bit saturation is an explicit lower/upper bound check after an explicit
  two's-complement `wrap32`, not the production `clip_intp2`.
* The reference never calls `rshift*`, `clip*`, `aptx_generate_dither`,
  `aptx_invert_quantization`, `aptx_prediction_filtering`,
  `aptx_process_subband`, `aptx_pack_codeword`, `aptx_unpack_codeword` or any
  other production helper to produce an expected value.
* Residual history is kept in plain chronological order (`residual[0]` is the
  newest sample). The production ring buffer with negative indexing and
  `pos` is not copied; where production layout is inspected in the predictor
  group, it is read by explicit index arithmetic and compared with the
  chronological model.
* Only the format constant tables (`all_tables`, `quantization_factors`) are
  read, as allowed.

The stream group does not re-encode to derive expectations. It reads the
codewords the encoder actually emitted from the output buffer, unpacks them
with the independent field plan and the model's parity equation, and feeds
those samples to the model. Encoder and decoder are driven one beat at a time
so each beat's state can be compared.

## Dither group

Covered state, per format (aptX and aptX HD):

* Codeword field plans, declared independently in `ref_fields_aptx` and
  `ref_fields_hd` (7/4/2/3 bits and 9/6/4/5 bits).
* 32-bit codeword history `history = (cw << 8) + (history << 4)`, including
  runs long enough to set the sign bit and wrap.
* All four dither subbands and the dither parity, derived from
  `5184443 * (history >> 7)` using floor division and explicit wrapping.
* Positive, negative, zero and extreme codewords in each subband.
* Packing and unpacking, including the highest subband's least significant
  bit carrying the parity and being restored on unpack.
* Big-endian byte serialisation round-trips independently.
* Eight-beat parity sync: the model predicts which beats need forcing, and
  `aptx_insert_sync` is checked to force exactly those beats, nudge exactly
  one sample (the smallest-error candidate in the mandated subband order), and
  leave the emitted total parity equal to the eighth-beat marker.
* Left and right channels maintain separate histories, both in the model and
  in two independently driven production channels.

## Predictor group

Covered for both formats and all four subbands, on every beat:

* The reconstructed difference from inverse quantization.
* `factor_select` and the next beat's quantization factor, with the rule
  asserted explicitly: the current symbol is reconstructed with the old
  factor, and the updated factor belongs to the next symbol.
* Both pole weights, the pole sign history, and `previous_reconstructed_sample`.
* Every zero weight for the subband's prediction order.
* `predicted_difference` and `predicted_sample`.
* 24-bit saturation, with a dedicated near-clip scenario.

Input schedules alternate signs, include silence and steady small values, jump
suddenly after stability, approach positive and negative full scale, sweep
through zero, and then run long enough (160 beats) to cross the residual
history wrap for the 24-tap low band as well as the shorter orders. After the
wrap, the production ring buffer is read by index and compared with the
chronological model.

## Stream group

Driven through the public `aptx_encode` and `aptx_decode`, for aptX and aptX HD:

* Sustained stereo 24-bit audio (400 beats) mixing quiet, loud, stepped and
  clipping material.
* Beat-by-beat comparison of the encoder's and decoder's codeword history,
  dither, dither parity, quantization factor, `factor_select`, reconstructed
  difference, pole weights, pole signs, previous reconstructed sample,
  predicted difference, predicted sample and zero weights against per-channel
  models fed only the emitted codewords.
* Encoder and decoder prediction state must agree beat by beat.
* Public consumed/written counts: 24 bytes consumed per beat, 4 bytes (aptX)
  or 6 bytes (aptX HD) written.
* The 90-sample latency: 1600 input samples produce 1510 output samples, and
  the first PCM frames appear during beat 22 at sample offset 2.
* Independent QMF synthesis (chronological tap history, shared only the
  coefficient tables) reproduces every emitted PCM frame after the latency.
* `aptx_encode_finish` with ample and with short buffers: short calls return 0
  and continue, the completing call returns 1, the padding is 23 beats, and
  the context is reusable afterwards.
* `aptx_reset` makes a decoder reusable and it starts in its latency period
  again.
* Two encoder instances interleaved in time each keep their own history and
  are compared with their own model; the two model histories must differ.

PCM is lossy, so the test does not require decoded output to equal the input;
it only requires the decoder output to match the independent model's
synthesis, and separately that it is not silent. No separate QMF analysis
reference is needed.

## First difference reporting

Every comparison is a `TASK_CHECK` with the tag, beat, subband and the
reference value next to the observed value, for example:

```
predictor: aptx-sub0 subband 0 beat 37 zero weight 12 expected -174823 got -174822
```

`task_support/main.c` prints this and exits 42 on the first mismatch, so the
first divergence is always reported directly rather than being reduced to a
final hash. The mutation builds in `task_support/target.sh` exercise this:

* `history.patch` shifts the codeword history by one bit less and is caught in
  the dither group at the codeword-history comparison.
* `lag.patch` reads the previous tap in the zero filter and is caught in the
  predictor group at the predicted-difference comparison on the first beat.

Both mutations still compile and are detected by content comparison, not by a
crash.

## Building and running

```
bash task_support/baseline.sh   # reference encoder/decoder output
bash task_support/target.sh     # groups, mutation detection, baseline
```

The baseline file remains unchanged, so `target.sh` ends with the original
`APTX_BASE` lines.

## Toolchain

Only a system C99 compiler and the standard library are used. No generated
files, external data, or Python are required at test time.
