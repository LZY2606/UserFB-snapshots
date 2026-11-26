---
layout: default
---

# Minimum-phase sinc converter

`SRC_SINC_MINIMUM_PHASE` (converter number 5) is a causal, fixed-ratio
low-latency sinc converter.  It is intended for live monitoring where the
early part of the anti-imaging/anti-aliasing impulse is more important than
linear phase.

The old converter numbers, behaviour, ABI and quality settings are
unchanged.

## Supported ratios and lifecycle

The first non-empty conversion after `src_new()` or `src_reset()` fixes the
ratio, defined as:

```text
src_ratio = output_sample_rate / input_sample_rate
```

Only fixed ratios in the inclusive range **0.5 to 2.0** are supported in
this first version.  In particular, `0.5`, `44100.0 / 48000.0`, `1.0` and
`2.0` are checked by `tests/minimum_phase_test.c`.

After the ratio is fixed:

- passing the same value to `src_process()` or `src_set_ratio()` continues
  the conversion;
- passing a different ratio returns `SRC_ERR_NO_VARIABLE_RATIO` before any
  input frame is consumed or output frame generated;
- a ratio outside `[0.5, 2.0]` returns the minimum-phase ratio error;
- `src_set_ratio()` cannot bypass the fixed-ratio rule;
- `src_reset()` clears the fixed ratio and all channel histories.

The other converters retain their existing variable-ratio behaviour.

## Causal time convention

Output frame `k` belongs to source coordinate:

```text
t(k) = k / src_ratio
```

Let `q = floor(t(k))`.  The convolution only reads input frames
`q, q-1, ..., q-19`.  Negative frame numbers are the zero-valued history
before reset.  Samples later than `q` are never read, so mutating an
undelivered future frame cannot change an already produced output prefix.

All channels use the same `(q, fraction)` position.  Only their delay
histories are separate.

The integer kernel is 20 taps long; polyphase rows are also 20 taps.  Its
effective support is therefore below **20 input frames**, comfortably
inside the allowed 256-input-frame support limit.

## End of input and FIR tail

`end_of_input` does not append zeros or stretch the programme.  The valid
output interval covers only source coordinates covered by actual input
frames.  To observe or collect the FIR tail after an impulse, the caller
must explicitly append zero-valued input frames and process them.

This convention makes impulse-response and energy tests deterministic:
feed one `1.0` frame followed by at least 19 explicit zero frames.

## Coefficient source

The checked-in `src/minphase_coeffs.h` coefficients are generated, not
copied from another resampler.  The generation source is pure C in
`src/minphase_gen.c` and contains its own radix-2 FFT implementation.  It
does not use Python, external FFT libraries or machine-private tables.

Regenerate the header with either:

```sh
cc -std=c99 -O2 src/minphase_gen.c -lm -o /tmp/minphase_gen
/tmp/minphase_gen src/minphase_coeffs.h
```

or the Bash driver:

```sh
bash baseline/regenerate-minphase-coeffs.sh
```

The generator:

1. designs a 21-tap Kaiser-windowed sinc prototype with cutoff
   `0.235` cycles/input-frame and Kaiser beta `8.6`;
2. computes its real cepstrum with the built-in FFT;
3. keeps the causal/positive-quefrency cepstrum;
4. exponentiates its FFT to obtain a minimum-phase spectral factor;
5. normalizes DC gain to unity;
6. samples the band-limited continuation at 257 polyphase fractions.

At ratio 1, phase zero uses the generated integer minimum-phase kernel
exactly.

## Measured accuracy

`tests/minimum_phase_test.c` uses long deterministic sine PCM and projects
the middle half of each result with sine/cosine correlations.  Enforced
bounds are:

| Check | Enforced bound |
|---|---:|
| `0.02` cycles/input-frame passband at ratios 0.5, 44100/48000, 1 and 2 | amplitude error at most 0.5 dB |
| ratio 0.5, `0.10` input frequency | amplitude error at most 0.5 dB |
| ratio 0.5, `0.40` input alias at output frequency `0.20` | at least 35 dB below input |
| ratio 2, image at `0.45` output cycles/frame from `0.10` input | at least 35 dB below the `0.05` main output |
| minimum-phase relation above -35 dB magnitude | phase residual below 0.08 rad in the portable finite-DFT test |
| ratio 0.5 energy in source coordinates `< 12` frames | more than 80% of total impulse energy |

Typical passband errors of the generated 20-tap coefficients are about
0.05 dB or less in the checked configurations.  The generated design also
has substantial alias/image margin above the enforced 35 dB thresholds.

The phase test does not peak-align or remove a fitted linear delay.  It
projects the measured impulse with sine and cosine basis functions, forms
`H(f) = C(f) - iS(f)`, obtains the measured phase with `atan2(-S, C)`, and
unwraps adjacent principal phase values by adding or subtracting `2*pi`.
An independent real-cepstrum/Hilbert derivation from the **measured
amplitude** gives the expected minimum-phase phase.  Recomputing the
production coefficient design is not used as the reference.

## Interfaces and example

The converter is available through `src_new()` plus `src_process()`,
`src_simple()`, `src_clone()`, `src_reset()` and the callback API.
`src_get_name(SRC_SINC_MINIMUM_PHASE)` and
`src_get_description(SRC_SINC_MINIMUM_PHASE)` recognize converter 5.

A dependency-free C example is provided in
`examples/minimum_phase_demo.c`.

The required Bash verification driver is:

```sh
bash baseline/run-target.sh
```

It builds and runs the five `minimum_phase_test` cases (`causal`,
`magnitude`, `phase`, `streaming` and `lifecycle`) and then runs
`simple_test`, `varispeed_test` and `multi_channel_test`.
