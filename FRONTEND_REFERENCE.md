# AMR-WB encoder front-end reference

This document describes the integer-exact reference model in
`tests/frontend_reference.c` and the contract tests in
`tests/frontend_contract.c`.  The model protects the first signal-processing
stage of the VisualOn AMR-WB encoder against future refactors that would
change the first numeric deviation.

## Sample coordinates

The encoder receives 320 samples per frame at 16 kHz.  `AMR_Enc_Encode`
first masks the low two bits of every input sample (`& 0xfffc`, a 14-bit
input).  The front end then works in two coordinate systems:

| name            | rate    | span                       | index in this model        |
|-----------------|---------|----------------------------|----------------------------|
| input samples   | 16 kHz  | 320 per frame              | `in16[0..319]`             |
| real output     | 12.8 kHz| 256 per frame              | `decim[0..255]`            |
| preview output  | 12.8 kHz| 12 extra points            | `preview[0..11]`           |
| HP / pre-emph   | 12.8 kHz| 268 points, real then preview | `hp[]`, `preemph[]`     |

`decim[j]` corresponds to input position `5*j/4`, i.e.

* integer input index `i = (5*j) >> 2`,
* polyphase index `frac = (5*j) & 3` (four phases, visited in the order
  0,1,2,3,0,…).

The 30-tap interpolation filter starts at internal window index
`i - 15 + 1`, where the internal window is
`[30 memory samples][input samples]`.  Equivalently, from the window base
`base`, output `j` reads taps `base[i+1 .. i+30]`.

## Why there is a preview

The LP analysis window is longer than the synthesised frame, so the
decimator and the 50 Hz filter need 12 extra output points beyond the 256
real ones.  The encoder produces them by running the same filters for 15
zero 16 kHz inputs after the real 256-point advance.  These points are
**preview**: they take part in the high-pass filter, in the pre-emphasis
maximum search and in `Q_new`, but they must not be committed to the
long-term filter memories used by the next frame.

The three quantities that make this distinction visible are:

* `mem_decim[30]` – the decimator's long-term memory is the last 30 **real**
  input samples; the preview decimation runs from a *copy* of it and its own
  memory is discarded.
* `mem_sig_in[6]` – the 50 Hz filter memory kept for the next frame is the one
  after the 256 real points; the copy left by the 12 preview points is
  discarded.
* `mem_preemph` – stores the last **real** high-pass point
  (`hp[255]`), not the last preview point (`hp[267]`).

## Fixed-point model

The model is written from the arithmetic specification, not by calling the
production routines (`Decim_12k8`, `HP50_12k8`, `Scale_sig`), using their
macros, or transcribing their unrolled loops.  All 32-bit operations go
through helpers with fully defined behaviour:

* `wrap_add` / `wrap_sub` / `wrap_shl` perform two's-complement arithmetic
  via `uint32_t`.
* `sar` is an explicit arithmetic right shift on the two's-complement
  representation (it does not rely on the implementation's `>>` on negative
  values).
* `sat_add` saturates a 32-bit sum.
* `l_shl` reproduces the saturating, one-bit-at-a-time left shift of the
  fixed-point library.
* `round_h(v)` is `extract_h(v + 0x8000)`; the `0x8000` offset is added with
  defined wrap-around, so exact midpoint ties are decided the same way the
  hardware does.
* `norm_s` reproduces the normalisation count used by the scaling search.

Filter coefficients are format constants: the four 30-tap Q14 polyphase rows
in `fir_down[4][30]`, and the Q12 high-pass `{4053, -8106, 4053}` /
`{8192, 16211, -8021}` (the `a[]` values are the doubled forms).  These are
data, not code paths, and live in a table in `frontend_reference.c`.

The high-pass filter uses the high/low word split.  Its recursion is
modelled exactly:

```
t   = 8192 + y1_lo*a1 + y2_lo*a2          (32-bit wrap)
t >>= 14                                   (arithmetic)
t  += 2 * (y1_hi*a1 + y2_hi*a2 + (x0+x2)*b0 + x1*b1)
t  = sat_l_shl(t, 2)                        (Q12 -> Q13)
y1_hi = t >> 16 ;  y1_lo = (t & 0xffff) >> 1
out   = extract_h(sat_l_shl(t,1) + 0x8000)
```

## Scaling: max search, Q_new and the two-frame Q_max clamp

The pre-emphasis scale factor is chosen from the **268** points (256 real +
12 preview):

```
mu    = PREEMPH_FAC >> 1                   (Q14)
max   = max_i | (hp[i]<<15) - 2*mu*hp[i-1] |
shift = 8 if (max>>16)==0 else clamp(norm_s(max>>16)-1, 0, 8)
Q_new = min(shift, Q_max[0], Q_max[1])
exp   = Q_new - Q_old
Q_old = Q_new
Q_max[1] = Q_max[0]
Q_max[0] = shift                            (uncapped, as production stores it)
```

So the clamp constrains the current frame using both stored maxima while the
stored maxima keep the *uncapped* shift.  `exp` is then used to rescale the
previous speech interval with `Scale_sig(old_speech, 116, exp)`.

## Frame state and the next `old_speech`

After pre-emphasis the model carries these exact quantities for comparison:

* `decim[256]`, `preview[12]`, `preemph[268]`;
* `mem_decim[30]`, `mem_sig_in[6]`, `mem_preemph`;
* `q_new`, `q_old`, `q_max0`, `q_max1`;
* `past_scaled[116]` – the old speech interval after `Scale_sig`;
* `next_old_speech[128]` – the 128-sample image the production code copies
  into `st->old_speech` for the next frame (`preemph[140..267]`).

The model owns its history in natural input coordinates and is never
refreshed from production state between frames.

## Observation points

Under `-DTASK_FRONTEND_TEST`, `amrwbenc/src/voAMRWBEnc.c` calls a read-only
observer at four points (`FE_STAGE_DECIM`, `FE_STAGE_HP`,
`FE_STAGE_PREEMPH`, `FE_STAGE_END`).  The observer only copies state out;
nothing is fed back.  When the macro is off, no observer, callback, extra
state or allocation exists.  `tests/frontend_contract.c` installs the
observer, drives the public API (`voAMRWB_SetInputData` /
`voAMRWB_GetOutputData`), and compares each stage against the model.

* DECIM runs after the real 256-point decimation and the 12-point preview.
* HP runs after both 50 Hz passes; it exposes the retained `mem_sig_in`.
* PREEMPH runs after the max search, scaling and old-interval rescale.
* END runs after the frame's `old_speech`/`old_wsp`/`old_exc` update.

## Streaming contract

`tests/frontend_contract.c` also checks the streaming buffer:

* Input split into 1, 3, 637 … byte chunks is presented one chunk at a time
  through `SetInputData`; the borrowed pointer is consumed before the next
  chunk (`INPUT_BUFFER_SMALL` until 640 bytes accumulate).  No observation
  record may appear before a frame is complete; once complete, the parameter
  trajectory and output bytes equal the whole-frame `E_IF_encode` path.
* A homing frame (all `0x0008`) is processed as a normal frame, then the
  encoder resets; the next two frames equal a freshly opened instance.
* Two interleaved instances stay independent, and any mismatch reports
  stream, frame, stage, sample coordinate and expected/actual value.

## Running

```
bash task_support/run.sh target      # build + run the four groups + baseline
bash task_support/mutants.sh         # normal run, then two mutation checks
```

`run.sh target` compiles all production sources plus `tests/*.c` as plain C.
`mutants.sh` copies the tree to a scratch directory twice, applies one
production mutation in each (`PreviewCommit`, `PreemphPreview`), and requires
both to build and be reported `FAIL` with exit status 4.
