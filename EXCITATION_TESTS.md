# Excitation path numerical regression

`tests/excitation_reference.c` is a self-contained, in-process numerical
regression for the bcg729 decoder **excitation path**: the adaptive codebook
vector, the fixed codebook vector and the per-sample excitation combination
that feeds the LP synthesis filter. It is driven by `task_support/driver.c`
through `task_support/check.h` and defines `task_cases` /
`task_case_count`; it contains no `main()`.

Run it with:

```sh
bash task_support/run.sh target
bash tests/check_excitation_mutants.sh
```

## What the reference proves independently

Every expected value is derived inside the test from standalone arrays,
64 bit integers and explicit fixed-point rounding. The production adaptive
codebook, fixed codebook and rounding macros (`PSHR`, `MULT16_16_P14`,
`SATURATE`, ...) are never used to build an expectation, and no output of
another bcg729 decoder instance is used as a reference. The interpolation
filter `b30` is re-declared in the test from the ITU G.729 3.7.1 table rather
than read from `codebooks.c`.

### pitch group (8 cases)

- Decodes the first subframe **absolute** pitch index itself: the integer
  delay and the `{-1, 0, +1}` fractional phase are computed with explicit
  integer division, including the 197 index boundary (below it the three-way
  fractional mapping, at/above it `k = P1 - 112`).
- Decodes the second subframe as a **relative** window on top of the first
  integer delay. The window base `tMin = k1 - 5` is clamped to `[20, 134]`
  before the relative delta is applied; dedicated cases hit both clamps.
- The sign of the fractional part selects the interpolation base point and
  the two `b30` phases; the 40 adaptive excitation samples are rebuilt one
  sample at a time as the two finite (ten-tap) convolutions.
- Short lags read samples generated inside the same subframe. The reference
  writes into the same buffer it reads from, in causal sample order, so the
  feedback is genuinely exercised instead of being convolved over a snapshot
  of old history.
- Every case checks all 40 samples of both subframes, both integer delays,
  `previousIntPitchDelay` and the untouched 154-word pitch history.
- Continuous-state handling covers a parity error (it changes only the first
  subframe; the second still decodes normally), an explicit erased frame in
  which each subframe pushes the previous integer delay forward by one, and
  the 143 cap. One erasure case makes both pushes observable (60→61→62),
  another starts at 142 to prove the cap (142→143→143).

### innovation group (5 cases)

- Decodes the four pulses directly from the 13-bit position word (three
  3-bit track fields plus the extra `jx` bit that chooses between the two
  last-pulse tracks) and the 4-bit sign word.
- Builds the base Q13 vector first, then applies the eq48 sharpening
  propagation in ascending sample order using the *input* bounded pitch
  gain, with explicit Q14 half-up rounding on 64 bit products.
- Compares all 40 samples (pulse values themselves, never just a pulse
  count) and separately verifies the propagation recurrence on the
  production output at legal speech lags.
- Coverage includes both `jx` tracks, mixed signs with non-trivial rounding
  on both sides of zero, the minimum legal lag 20 at maximum bounded gain,
  a pulse position that collides with a propagated contribution, and lag 40
  where no propagation is possible.

### frames group (1 case, seven frames, two instances)

- Drives the public `bcg729Decoder()` over the normal ten-byte speech path.
  Bitstream fields, including the P1 parity bit, are planned from the field
  definitions; the encoder is never used to produce an expectation.
- Both subframes of every frame are synthesised sample by sample by the
  reference. After each frame the reference history is shifted exactly like
  the decoder, so frame-to-frame excitation history is derived independently
  across the whole sequence.
- The fixed codebook is sharpened with the bounded gain as it was **before**
  the subframe; the newly bounded gain is retained only for the next
  subframe, matching the decoder ordering.
- The checks compare the 80 excitation samples that survive the decoder
  shift, the full retained 154-word pitch history, `previousIntPitchDelay`
  and the bounded gain, and require continuously non-zero PCM on every
  frame. The sequence contains subframes where the new bounded gain differs
  from the previous one, and two decoder instances are processed
  interleaved to prove their state does not cross.

## Boundaries of the independent model

The regression is deliberately scoped to the excitation arithmetic and its
state handling.

- **Gain quantiser out of scope.** `decodeGains()` (the GA/GB tables, the MA
  gain prediction and the prediction-error memory) is not re-derived. The
  frames group calls the production `decodeGains()` on a **separate decoder
  context** that exists only for that purpose, feeding it the reference fixed
  vector. Its outputs are the sole production numbers consumed by the
  reference; the context under test is never read for an expectation.
- **LSP/LPC out of scope.** LSP decoding, interpolation and `qLSP2LP` are
  exercised only because the public decoder needs them; they are not
  independently proven. The fixed, legal LSP fields are constant across the
  frame plans.
- **LP synthesis filter, post-filter and post-processing are not
  re-implemented.** PCM is only checked for being continuously non-zero; the
  test does not claim lossy PCM equals any input signal.
- **No SID frames, RFC 3389 aggregation payloads or comfort-noise paths.**
  Only ordinary active speech frames, parity errors and explicit erasures are
  covered.
- Inputs are deterministic, small-amplitude synthetic histories and legal
  parameter words; no external speech vectors or golden files are needed.

## Gap relative to the legacy block tests

The pre-existing programs under `test/src/` (for example
`decodeAdaptativeCodeVectorTest.c`, `decodeFixedCodeVectorTest.c` and
`decoderTest.c`) are file-driven dumpers:

1. they read CSV inputs produced outside the program and write CSV/PCM
   outputs, with no in-process pass/fail assertion — validation happens by
   diffing against externally supplied golden vectors;
2. each invocation reloads the complete excitation buffer from the input
   file, so the excitation history that flows between the two subframes of a
   frame and between frames is never independently derived inside the
   program;
3. they check exported block outputs, not the integer/fractional delay
   decoding, the `previousIntPitchDelay` transitions, the bounded-gain
   ordering or the causal short-lag feedback;
4. nothing asserts that a deliberately injected algorithmic change is
   detected.

This regression closes those gaps by deriving all excitation state in
process, asserting every sample and the state variables, and by the mutation
check below.

## Mutation check

`tests/check_excitation_mutants.sh` builds two compilable variants in
temporary copies of the tree (the original sources are checksummed and never
modified):

- **fractional base point** — in `decodeAdaptativeCodeVector.c` the positive
  fractional adjustment `-(intPitchDelay+1)` is inverted to
  `-(intPitchDelay-1)`;
- **fixed codebook track bit** — in `decodeFixedCodeVector.c` the last-pulse
  `jx` selection bit is XORed with 1.

Each variant must produce a `CONTENT_MISMATCH` line and the runner must exit
with status 4. A compile failure or a crash (status ≥ 128) is explicitly
rejected as a detection. The script creates its working directories under
`TMPDIR` from the process id and removes them on exit, so it does not depend
on environment-specific `mktemp` behaviour.
