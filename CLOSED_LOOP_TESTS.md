# Closed-loop regression tests for the GSM 06.10 fixed-point coder

`tests/closed_loop.c` is an independent, explainable regression suite for
the three sections of `libgsm` that are about to be optimized:

* **ltp** — long term predictor lag/gain selection, the prediction and the
  residual subtraction (`src/long_term.c`);
* **rpe** — weighting filter, grid selection, APCM quantization and sparse
  reconstruction (`src/rpe.c`);
* **loop** — the four-subframe closed encoder loop, including what is fed
  back into the reconstruction history (`src/code.c`).

The fixed runner in `task_support/driver.c` discovers the cases through
`task_cases` / `task_case_count` declared in `task_support/check.h`.
The test defines no `main()`; a passing run returns 0 and every comparison
goes through `TASK_CHECK`/`task_check`.

## Why the old tests are not enough

* `tst/run` compares against `1.inp`/`1.cod`/`1.out` speech vectors that are
  explicitly missing from this distribution ("we cannot provide the test
  data with this release"), so it cannot run here at all.
* `task_support/baseline.c` can run continuous encode/decode, but it only
  keeps two FNV-style hashes of the byte stream and the decoded PCM plus a
  non-zero count.  A hash cannot explain *which* decision changed: it cannot
  distinguish a wrong LTP lag from a wrong RPE grid, a wrong gain level from
  a wrong pulse, nor can it tell whether the encoder fed back a reconstructed
  residual or the original one.  The hashes are still asserted in
  `task_support/run.sh` for backward compatibility, but they are not used as
  a numerical reference by these new tests.
* Nothing in the old suite pins the tie-breaking rules (first lag on equal
  correlation, smaller grid number on equal energy), the all-non-positive
  correlation fallback, the exponent/mantissa edge cases, or the exact
  closed-loop feedback value.

## How the expected values are produced

Every expected scalar and vector comes from a small reference model written
directly in `tests/closed_loop.c`.  It uses only C `long` (at least 32 bit)
intermediates and spells out each fixed-point rule explicitly:

* arithmetic shift right for division with rounding towards minus infinity;
* `GSM_MULT`/`GSM_MULT_R` style floor and rounded products, including the
  `(-32768,-32768)` special case;
* explicit 16-bit saturation for additions/subtractions;
* the exact `gsm_norm`, `gsm_asl` (negative-shift fallback), xmax exponent
  coding and exponent/mantissa decoding rules.

The reference never calls the production LTP, RPE, quantization or
dequantization functions, and its loops are ordinary `for` loops rather than
the manually unrolled production loops.  The only production code exercised
is the system under test, plus (in the loop group, by design) the
preprocessing/LPC/short-term front end on a separately initialized context.
Format constants are duplicated from `src/table.c` with their table numbers.

### ltp group

Each case supplies an explicit 40-sample short-term residual and a
120-sample reconstruction history.  The reference independently:

1. derives the scaling from the residual peak magnitude;
2. sweeps every lag 40..120 with a strict maximum test, so equal positive
   correlations keep the first lag and an all-non-positive surface keeps
   `Nc = 40`;
3. derives `bc` from the selected history power and the normalized
   correlation;
4. recomputes the prediction `dpp` and the subtracted residual `e`.

The cases cover a scaling switch, a repeated-period tie (lags 40/80/120),
all-non-positive correlation, a zero residual and gain levels 0, 1, 2 and 3.
The coverage case prints and asserts how many times each situation was hit.
Each case checks `Nc`, `bc`, all 40 predictions and all 40 residuals against
`Gsm_Long_Term_Predictor`, not merely that the lag is in range.

### rpe group

The input is a 40-sample residual padded with five zeros on each side.  The
reference performs the finite 11-tap weighting convolution, evaluates all
four 3-fold decimated grids (grids 0 and 3 share 12 positions but differ at
the endpoints), and keeps the smaller grid number on equal energy.  It then
derives `xmaxc`, the exponent/mantissa, the thirteen 3-bit pulses and
rebuilds the 40-sample sparse residual from the explicit inverse
quantization and grid positioning rules.  The cases include endpoint-driven
grid 0/grid 3 wins, an exact grid-0/grid-3 energy tie, an all-zero input,
negative odd samples, a full-scale input that saturates the weighting
filter, and several exponent levels.  The decoder side of the library is not
used as an oracle.

### loop group

Deterministic PCM (silence, a periodic square wave followed by an abrupt
change to noise, and a second independent pattern) is fed for six frames.
Preprocessing, LPC and short-term analysis are out of scope for independent
modeling and run through the original functions on their own freshly created
context; everything from the LTP onward is advanced by the reference itself.
Crucially, each subframe appends **quantized reconstructed residual plus
prediction** to the history (never the original short-term residual), and
only the last 120 reconstructed samples survive.  Two distinct instances run
interleaved frame by frame.  Every frame compares the reference fields with
both the `Gsm_Coder` outputs and the fields recovered from the public
`gsm_encode` + `gsm_explode` 33-byte path, and compares the coder's retained
reconstruction history sample by sample.

WAV49 chaining, floating-point/`FAST` approximations and `LTP_CUT` are
deliberately out of scope; the ordinary 33-byte frame format and the integer
implementation are what is modeled.

## Mutation testing

`tests/check_closed_loop_mutants.sh` builds two temporary source copies
(the original tree is never written to) and applies one mutation per copy:

1. `long_term.c`: the strict LTP correlation maximum becomes `>=`;
2. `code.c`: the closed-loop feedback is changed to the current original
   short-term residual instead of the quantized reconstructed one.

A mutant counts as detected only when it still compiles and the same runner
rejects it with status 4 after printing `CONTENT_MISMATCH`.  A build failure
or an abnormal termination is reported as a failure of the test, not as a
detection.  Failures are printed with `fprintf`, followed by a
`TASK_CHECK(0)` that makes the fixed runner return 4.

## Running

```sh
bash task_support/run.sh target
bash tests/check_closed_loop_mutants.sh
```

Only a system C compiler, a POSIX shell and the standard library/utilities
are required; no data files or network access are needed.
