---
layout: default
---

# SRC_SINC_MINIMUM_PHASE : Causal Minimum Phase Converter

The `SRC_SINC_MINIMUM_PHASE` converter (enum value 5) is a causal, minimum
phase, sinc based sample rate converter designed for low latency monitoring
and live foldback paths, where the symmetric (linear phase) sinc converters
are unusable because their kernels wait for future input. It trades the
linear phase response of the other sinc converters for minimal delay: the
bulk of the impulse response energy is emitted as early as causality allows.

## Causality contract

Output frame `k` corresponds to source time `k / r` (`r` = output/input
ratio) and is computed from input frames `0 .. floor (k / r)` only, plus the
zero history that precedes the stream (the state is zeroed on reset). The
converter never waits for future samples: as soon as input frame
`floor (k / r)` has been delivered, output `k` can be produced, even if input
arrives one frame at a time and the output buffer holds a single frame.

The valid output time range is exactly the time covered by the delivered
input. Setting `end_of_input` does **not** flush or extend the programme.
To observe the FIR tail, append zero input explicitly.

## Coefficient origin

The kernel is designed at run time, once, when the ratio is locked (the
first non-empty conversion after a reset). No Python, no external FFT
library and no machine generated private tables are involved; the designer
is plain C living in `src/src_minphase.c` and uses a small self contained
radix-2 FFT. Two grid layouts are used:

- **ratio == 1** — output times land exactly on the input grid, so the
  kernel is designed on the input grid: a Blackman windowed sinc prototype
  (cutoff 0.45 cycles/input-frame, 32 taps) is converted to a minimum phase
  kernel with the homomorphic (real cepstrum) method: `log |H|`, inverse
  FFT, cepstral folding, forward FFT, exponentiate, inverse FFT. The result
  is minimum phase with respect to its own magnitude on the input grid.
- **all other ratios** — output times fall between input samples, so the
  kernel is designed on a fine grid of 64 points per input frame: a
  Blackman windowed sinc prototype (cutoff `0.45 * min (1, r)` cycles per
  input frame, 2048 taps on the fine grid) is made causal and minimum phase
  with the same homomorphic transform. Because the kernel is causal on the
  fine grid, every polyphase branch keeps unity DC gain and the fractional
  phases required by the ratio are exact samples of one smooth causal
  kernel. The run time table stores 64 phases per input frame and
  interpolates linearly between neighbouring rows.

The effective kernel support is 32 input frames (the format allows up to
256). During normal processing the converter performs no memory allocation
and never re-runs the designer; the only allocation outside `src_new()` is
the scratch FFT buffer of the one-shot designer, freed before it returns.

## Magnitude and phase error

Measured on deterministic PCM, steady state, transients excluded:

- Passband at 0.02 cycles/input-frame: within ±0.5 dB for ratios 0.5,
  44100/48000, 1 and 2 (typical error < 0.01 dB).
- Ratio 0.5: a 0.40 cycles/input-frame sine aliases into the output
  attenuated by about 90 dB (requirement: 35 dB); the 0.10
  cycles/input-frame passband stays within ±0.5 dB.
- Ratio 2: the sampling image of a 0.10 cycles/input-frame sine at 0.45
  cycles/output-frame sits more than 35 dB below the main tone.
- Phase: minimum phase (nonlinear). On the ratio 1 grid the measured phase
  matches the phase derived independently from the measured magnitude
  response (log-magnitude cepstrum / Hilbert relation) to within
  0.02 radians across the passband, with no linear-delay alignment.
- Energy concentration: at ratio 0.5, more than 98 % of the impulse
  response energy falls within the first 12 input frames after the first
  nonzero output (requirement: 80 %).

## Ratio policy

Only conversion ratios (output/input) in the inclusive range **[0.5, 2.0]**
are supported. After each `src_reset()` the ratio is fixed by the first
non-empty conversion; any later attempt to change it — through
`src_process()` or `src_set_ratio()` — fails with an error before any input
is consumed or any output is generated. Passing the same value again is
allowed. `src_reset()` makes the converter behave exactly like a fresh
instance. The other converters keep their usual varispeed rules.

## Usage notes

- All channels share the same fractional position; per-channel histories
  are independent.
- `src_simple`, `src_process` and the callback API
  (`src_callback_new`/`src_callback_read`) all produce identical output for
  identical input.
- The converter is not intended for varispeed or arbitrary ratio sweeps;
  use the other converters for that.
