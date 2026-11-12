# Numeric filter contract

`filter_contract.c` checks the observable signal-processing behaviour of
libsoxr through the public `soxr_create` / `soxr_process` / `soxr_oneshot`
API only.  Every conversion uses `SOXR_VHQ`, `SOXR_FLOAT64` in and out, and a
single-threaded runtime (`soxr_runtime_spec(1)`).  Linear phase is phase 50;
minimum (0) and maximum (100) phase are set explicitly.  The reference side is
fully independent: analytically generated sine waves, direct least-squares
fits, folding equations and explicit summations.  It never reuses the library
under test, library coefficient tables or internal FFT routines.

Run one group with `filter-contract {passband|alias|phase}`.  On success it
prints `PASS <group> checks=<n>`, where `n` counts the assertions evaluated.
Failures print the parameters, the observed quantity and the tolerance.

## Stimuli and accounting

* Inputs are steady sine waves `x[i] = A sin(2 pi f i / Fs_in)` of 2-4 s, so
  both head and tail transients can be skipped.
* Every case asserts the error return is null, that **all input frames** are
  consumed and that the output frame count equals the analytic value
  `round(Fs_out * N_in / Fs_in)`.  Streamed cases finish with `in = NULL`
  drain calls until zero output is returned.
* Frame counts are per channel; the two-channel cases keep sample and frame
  bookkeeping separate and exercise both interleaved and split layouts.
* A zero input rate is asserted to be rejected by `soxr_create`.

## Amplitude and frequency fit

Over a window starting at `w0` with `N` frames, fit

```
x[k] = b sin(w k) + d cos(w k),          w = 2 pi f0 / Fs_out
```

by solving the 2x2 normal equations directly (no FFT).  Then
`A = hypot(b,d)`, phase `= atan2(b,d)`.  When the window holds whole cycles
the basis is orthogonal and this is exactly a DFT-bin projection; the general
normal-equation solution is retained so non-exact bins stay unbiased.

Frequency is estimated independently from the phase drift between fits on two
half windows of `N/2` frames:

```
f = f0 + (phi2 - phi1) / (2 pi dt),     dt = (N - N/2) / Fs_out
```

The analysis window is the central **1 second** of the output
(`N = Fs_out` frames), which gives a 1 Hz Fourier-bin spacing; all stimulus
frequencies are integer Hz, so each window contains whole cycles and no
windowing taper is needed.  The phase estimator resolves well below one bin
(measured error below ~1e-3 Hz; tolerance 1e-3 Hz).

Pass-band amplitude tolerance is a relative error of 1e-5; the VHQ FLOAT64
build measures around 1e-10, leaving a wide margin.  One-shot and irregular
chunked streaming are checked against the same analytic limits but are not
required to be bit-identical to each other.

## Aliasing and imaging

Residual frequencies come from the sampling folding equations, evaluated
independently of the library:

* 48 -> 32 kHz, an 18 kHz tone folds about the 16 kHz output Nyquist:
  `f_alias = 2*16000 - 18000 = 14000 Hz`.
* 32 -> 96 kHz, a 10 kHz tone images about the 32 kHz input sampling
  frequency at `32000 - 10000 = 22000 Hz` and `32000 + 10000 = 42000 Hz`.

Each residual is fitted over the same integer-cycle 1 s window (never a single
sample) and its amplitude relative to the unit-amplitude input must stay below
`10^(-75/20)` (-75 dB).  The same cases confirm the wanted pass-band tone
survives at unit amplitude.  Boundary cases cover a constant (DC) input
(unity gain, residual ripple <= 1e-9) and an all-zero input (exactly zero
output with the full frame count).

## Phase response

For 48 -> 44.1 kHz, a unit impulse is placed at input frame 24000 and all
44100 output frames are collected.  With an ideal linear-phase resampler the
impulse response is centred on output frame `22050` (= 24000 * 44100/48000).
Metrics, each from an independent direct summation:

* Energy `E = sum h[n]^2` and energy centroid
  `c = sum n h[n]^2 / E - 22050`, using **every** valid output frame.
  Linear: |c| < 1e-3 frame; minimum: c > 1; maximum: c < -1.
* Linear-phase symmetry
  `sqrt(sum_k (h[22050-k] - h[22050+k])^2 / E) < 1e-9`.
* Relative spread of total energy across the three phase designs < 1e-5.
* Pass-band magnitude at 1/6/10 kHz from an independent complex-exponential
  projection `H(f) = (2/N) sum h[k] exp(-j 2 pi f k / Fs)`; the spread across
  phase designs must be below 0.02 dB.

A support window is defined by the 1e-7-of-peak envelope; the energy falling
outside it is reported as a tail fraction and asserted below 1e-9, giving
evidence that window truncation discards negligible energy.

## Error sources and margins

Dominant residual terms are FLOAT64 round-off in the polyphase/DFT engine and
in the reference summations, plus the small pass-band ripple of the VHQ
design.  Measured healthy values and test limits:

| metric                        | measured            | limit      |
|-------------------------------|---------------------|------------|
| pass-band amplitude rel. err. | ~1e-10              | 1e-5       |
| frequency error               | < 1e-3 Hz           | 1e-3 Hz    |
| alias/image level             | < -190 dB           | -75 dB     |
| linear centroid offset        | 0 (exact to 1e-16)  | 1e-3 frame |
| min / max centroid offset     | +3.78 / -4.70 frame | +/-1 frame |
| phase energy spread           | ~1e-10              | 1e-5       |
| pass-band magnitude spread    | micro-dB            | 0.02 dB    |

## Mutations

`check-mutations.sh` builds throwaway copies of the source with one numerical
degradation each and requires the matching group to reject it numerically
(compile failures, library errors and empty output fail the script itself):

| mutation | location | detected by |
|----------|----------|-------------|
| low-pass coefficient gain x 0.99 | `lsx_make_lpf` in `src/filter.c` | passband (1% gain error) |
| design attenuation forced to 24 dB | `_soxr_init` in `src/cr.c` | alias (residual rises to ~-29 dB) |
| phase response forced to 50 | `_soxr_init` in `src/cr.c` | phase (all centroids collapse to 0) |
