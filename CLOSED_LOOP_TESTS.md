# Closed loop regression tests

`tests/closed_loop.c` is an independent, C-only regression suite for the
libgsm fixed point RPE-LTP *encoder*.  It exists to give the planned
optimisation of long term prediction and RPE a set of checks whose
expected values come from an explicit model, not from another copy of
the code being optimised.

## What is modelled, and what is not

The suite models three layers:

* **ltp** — lag selection, gain quantisation and the long term
  prediction / subtraction for one 40 sample subframe.
* **rpe** — the weighting filter, grid selection, APCM quantisation,
  inverse quantisation and grid positioning for one subframe.
* **loop** — the closed analysis/reconstruction loop across frames.

The *front end* of the encoder is deliberately **not** modelled:

* preprocessing (`Gsm_Preprocess`),
* LPC analysis (`Gsm_LPC_Analysis`),
* short term analysis filtering (`Gsm_Short_Term_Analysis_Filter`).

Those three routines are taken from the library and run on a context
owned by the reference model (`struct cl_loop.front`), separate from the
context that `Gsm_Coder` / `gsm_encode` use.  They are the only
production routines used to produce reference inputs.  In particular no
LTP, RPE, quantiser or dequantiser routine is ever called to compute an
expected value, and none of the unrolled production loops is reproduced
— the model uses plain loops of its own.

Everything after short term analysis is advanced independently:

* the LTP history receives the **quantised reconstruction**
  (`ep + dpp`), never the original short term residual;
* the grid positioning writes `xMp` at `Mc + 3*i` and zeros elsewhere;
* only the last 120 reconstructed samples survive into the next frame.

## Numeric conventions

Expected values are written with explicit, portable arithmetic:

* arithmetic right shift is `floor(v / 2^n)`, implemented with 64 bit
  division so it is independent of the host's behaviour for negative
  `>>`;
* products that the production code performs as 16 bit fractional
  multiplies are clamped to a signed 16 bit word after an explicit
  rounding or truncation rule;
* additions and subtractions that the production code saturates
  (`GSM_ADD` / `GSM_SUB`) are clamped to `[-32768, 32767]`;
* gain decisions use the **truncating** `gsm_mult` rule for the
  `DLB[]` thresholds; prediction and reconstruction use the
  **rounding** `gsm_mult_r` (`QLB[]`) rule.  Getting this pair the
  wrong way round changes results at the gain-decision boundary.

Output words, `Nc`, `bc`, `Mc`, `xmaxc` and every residual sample are
therefore compared to explicitly computed values, not merely range
checked.

## Group coverage

### ltp

The group starts from an explicit 40 sample residual and a 120 sample
reconstructed history and traverses every lag from 40 to 120.  Fixtures
hit:

* a **scaling switch** — one vector selected with `scal = 5` (lag 60) and
  one with the maximum `scal = 6` (lag 73), so the residual scaling
  search runs at both settings;
* a **repeated period** whose history repeats every 10 samples, so lags
  40, 50, … 120 all achieve the same positive correlation; the check
  requires the first encountered lag (40) to win and verifies that a
  non-strict `>=` scan would have chosen a different lag;
* **all correlations non-positive**, requiring lag 40 and gain 0;
* **four gain levels** (bc 0, 1, 2, 3) driven by scaled copies of the
  same residual.

Each case compares `Nc`, `bc`, the predicted samples `dpp[0..39]` and
the residual `e[0..39]`.  The report case prints and checks how many of
these situations were actually hit, so a fixture that silently degrades
cannot still pass.

### rpe

The residual is padded with five zero samples on each side, the
weighting filter is computed as a plain finite convolution over
`H[0..10]`, and the four three-fold decimated grids are compared by
energy.  Grid 0 and grid 3 share twelve positions (3..36) but use
different endpoints (position 0 versus position 39); the fixtures
include:

* **endpoint flips** in both directions: with grid 0 winning only
  because of position 0, and grid 3 winning only because of position
  39 — each constructed so that removing the endpoint would change the
  winner;
* an exact **energy tie** between grid 0 and grid 3, where the smaller
  grid number must be kept, and a direct energy tie between grids 1 and
  2;
* an **all-zero** residual (xmaxc 0, exponent -4, mantissa 7);
* a **negative odd** reconstructed sample, exercising the `<<1 - 7`
  sign restore;
* an **amplitude exponent switch** at xmaxc 15/16, where the decoded
  exponent changes from 0 to 1.

Each case compares `Mc`, `xmaxc`, all 13 `xMc` and all 40
reconstructed samples.  The library decoder is never used as the
reference; the reconstruction is derived from the model's own
inverse-quantisation and positioning.

### loop

Deterministic PCM is fed for at least six frames per instance, including:

* a periodic run followed by an abrupt change of waveform, which is
  asserted to move the LTP decision;
* two instances driven from different signals and interleaved frame by
  frame, whose reconstructed histories must diverge.

For every frame and subframe the suite compares:

* the reference fields against `Gsm_Coder`'s fields,
* the same fields against what `gsm_encode` followed by `gsm_explode`
  returns through the public API,
* the reference `dp0[0..279]` against the coder context's history.

The public API is exercised in its normal **33 byte** frame format only.
WAV49, the floating point approximation and `LTP_CUT` are out of scope.
No total hash is used as a numeric reference; hashes only survive in the
old baseline, which is kept for compatibility.

## Failure reporting

A numerical deviation is reported by `fprintf` (through the `cl_mismatch`
helper) and prints:

```
CONTENT_MISMATCH
PHASE <group> <case> frame=<n> sub=<m> field=<name> model=<a> lib=<b>
```

`PHASE` carries the group, the case tag, the frame and subframe, and the
first field or sample index that differed.  The case then records a
failed `TASK_CHECK(0)`, so `task_support/driver.c` exits with status 4.

## Mutation check

`tests/check_closed_loop_mutants.sh` verifies that the suite actually
constrains the two behaviours the optimisation will touch.  In a
temporary copy of the source tree it builds:

1. an **LTP mutant** where the strict correlation maximum comparison
   `L_result > L_max` becomes `>=` (so equal maxima prefer the later
   lag);
2. a **feedback mutant** where `code.c` feeds the current original
   short term residual back into the LTP history instead of the
   reconstructed residual.

Both mutants must compile, run to completion, and be rejected by the
suite with `CONTENT_MISMATCH` and exit status 4.  A build failure, a
crash or any other exit status is reported as a *miss*, not a detection.  The
script never modifies the real `src/`, `inc/`, `tests/` or
`task_support/`.

## Gap in the older tests

`tst/run` depends on speech vectors (`1.inp` … `4.cod` …) that are not
in the distribution, so it cannot run as shipped.  The bundled
`task_support/baseline.c` runs twelve frames of encode/decode and asserts
a total hash, a pcm hash and a nonzero-sample count.  That proves only
that the loop is *stable* and byte-for-byte unchanged; it cannot
localise a wrong prediction lag, a wrong grid selection, or a wrong
feedback path, and a hash difference gives no phase or field information.

This suite fills that gap: it checks the LTP lag, gain, prediction
residual and subtraction; the RPE grid, amplitude code, pulse codes and
sparse reconstruction; and the full closed loop feedback, per frame and
per subframe, against explicitly computed expectations.

## Files

* `tests/closed_loop.c` — the suite (no `main`, registered cases).
* `tests/check_closed_loop_mutants.sh` — the mutation check.
* `tests/patch_source.c` — literal patcher used only by the mutation
  check inside its temporary copy.

Run:

```sh
bash task_support/run.sh target && bash tests/check_closed_loop_mutants.sh
```
