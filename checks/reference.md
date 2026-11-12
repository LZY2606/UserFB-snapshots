# DSPFilters Butterworth numeric checks — reference notes

This document explains the independent mathematics behind `checks/target.cpp`
and the mutation harness `checks/mutations.sh`. The library exposes two
numeric paths that must each be pinned down on their own:

1. the graphical frequency response, computed from the biquad coefficients by
   `Cascade::response()`;
2. the actual audio path, computed from the section coefficients and their
   running state by `SimpleFilter::process()`.

Agreement between those two would not prove either one correct, so the
expected values are derived a third way, entirely inside `namespace ref` in
`target.cpp`. That code never reads a production coefficient, never calls a
production transform and never feeds a production response back in as an
expectation.

## 1. Analog prototype

The Butterworth low-pass prototype of order `N` has `N` poles equally spaced
on the left-half unit circle, at angles

```
theta_k = pi/2 + (2k+1) pi/(2N),  k = 0..N-1.
```

Its transfer function is normalized to unity at DC:

```
H_a(p) = 1 / prod_k (p - p_k),   p_k = exp(j theta_k),   H_a(0) = 1.
```

For odd `N` one pole is real (`p = -1`); the remaining poles are conjugate
pairs. The reference builds the pole list directly from the angle formula
rather than reading `Butterworth::AnalogLowPass`.

## 2. Bilinear prewarping and frequency mapping

The bilinear transform maps the digital unit circle `z = exp(j 2 pi f)`
(`f` in cycles/sample, `0 < f < 1/2`) onto the imaginary analog axis via

```
s = j omega,   omega = tan(pi f).
```

Every digital edge frequency is prewarped through this `tan` before it enters
an analog mapping. This is what keeps the designed cutoff exactly on the
analog -3 dB point despite the nonlinear frequency axis; a linear `pi f`
substitution is the deliberately broken case in `mutations.sh`.

### 2.1 Low pass

With `omega_c = tan(pi fc)`:

```
p = s / omega_c,          H_LP(f) = H_a(j omega / omega_c).
```

### 2.2 High pass

```
p = omega_c / s,          H_HP(f) = (-1)^N H_a(j omega_c / omega).
```

The factor `(-1)^N` is a fixed sign convention of the shipped library (its
`HighPassTransform` maps the prototype infinity zero to `z = +1` and the
prototype DC point to `z = -1`), and is applied unconditionally for the
high-pass response only. It is hard-coded from the convention, never inferred
from a measured response or audio run, and the full complex value is retained
(no magnitude-only comparison that would discard the phase).

### 2.3 Band pass and band stop — separately prewarped edges

For digital center `fc` and width `fw`, the two digital edges are

```
f1 = fc - fw/2,   f2 = fc + fw/2.
```

They are prewarped **separately**:

```
omega1 = tan(pi f1),  omega2 = tan(pi f2),
omega0 = sqrt(omega1 omega2),     // analog geometric center
B      = omega2 - omega1.         // analog bandwidth
```

The analog center is the geometric mean of the two prewarped edges. Using the
prewarp of the digital center `tan(pi fc)` as the analog center would be
wrong, because `tan` is nonlinear; the digital frequency corresponding to
`omega0` is `atan(omega0)/pi`, which is included as a sweep feature point.

The prototype substitutions are then

```
band pass:  p = (s^2 + omega0^2) / (B s),
band stop:  p = B s / (s^2 + omega0^2),
```

evaluated on `s = j omega`, where both expressions are purely imaginary, so
the same `H_a` routine evaluates them. No extra sign factor is applied to
either band response.

### 2.4 Analytic DC / Nyquist limits

The endpoints are singular for `tan` and are therefore handled separately with
the analytic limits rather than the interior sweep.

| response | DC, f = 0        | Nyquist, f = 1/2     |
|----------|------------------|----------------------|
| low pass | 1                | 0                    |
| high pass| 0                | (-1)^N               |
| band pass| 0                | 0                    |
| band stop| 1                | 1                    |

## 3. Digital poles, order and stability

The same algebra is expressed directly in `z` using `s = (z-1)/(z+1)`:

```
low pass:  z = (1 + omega_c p) / (1 - omega_c p)
high pass: z = (p + omega_c) / (p - omega_c)
```

For the band transforms each analog pole produces a quadratic in `z` (from
substituting `s = (z-1)/(z+1)` into the band-pass/band-stop equation), giving
two digital poles per prototype pole. Consequently a band filter built from
an order-`N` prototype has **2N** digital poles and `N` biquad stages, not `N`
poles. Low/high-pass filters keep `N` poles, with one real single-pole section
when `N` is odd. Every independently computed pole is matched against the
production pole/zero layout, and every pole is asserted to lie strictly inside
the unit circle.

## 4. PCM steady-state observation

For an input sinusoid `x[n] = A cos(w n + phi)`, the steady-state output of a
filter with complex response `H(f)` is

```
y_ss[n] = Re( A exp(j phi) H(f) exp(j w n) ),   w = 2 pi f.
```

Each probe frequency is snapped so that its period in samples is an integer;
the observation window length is the LCM of the two channel periods times a
fixed number of cycles, making the window whole-period for both channels at
once. The steady-state complex amplitude is recovered by synchronous
projection over that window:

```
Ccos = (2/L) sum_n y[n] cos(w n),   Csin = (2/L) sum_n y[n] sin(w n),
K    = Ccos - j Csin.
```

Over an integer number of periods the orthogonal tone contributes no leakage,
so this is a phase-sensitive measurement — total energy alone would not catch
a sign or phase error. Each channel uses its own frequency, amplitude and
phase, and the expectation comes only from the independent `ref::response`.

The settling time before the window is estimated from the slowest digital
pole radius `r` using the envelope `r^n`, choosing `n` so the residual
transient is below `1e-4` (or `1e-3` for poles extremely close to the unit
circle), and is never shorter than four joint periods. Runs cover one-shot
processing, irregular chunk sizes 1/7/31, and a mid-stream `reset()` followed
by a full replay from pristine input.

## 5. Error sources and tolerances

* **Floating-point round-off.** Both paths use `double`; the independently
  derived response and the production biquad evaluation agree to about
  `2.5e-14` in the observed runs. The interior sweep tolerance is set to an
  absolute `2e-8` with a `1e-9` relative term — several orders of magnitude
  looser than the measured round-off, but tight enough that either mutation
  (which moves results by `1e-7` to `3e-2`) is detected immediately.
* **Near-zero responses.** Where the reference magnitude is below `1e-7`, a
  pure relative tolerance is meaningless, so an absolute tolerance is used.
  Endpoint hard zeros use a separate `1e-8` absolute residual check; endpoint
  unity/sign limits use `1e-12`.
* **Magnitude and complex difference.** Away from zero, both the full complex
  difference and the magnitude difference are asserted. A dB-only check would
  hide phase errors and is deliberately not used.
* **Anti-denormal bias.** The library injects an alternating `1e-8` term into
  the first section of every cascade (`DenormalPrevention`). Accumulated
  through the sections and the projection window this leaves a small residual
  in deep stop/notch bins. Stopband bins whose expected amplitude is below
  `1e-5` therefore use an absolute PCM threshold of `5e-6`; all other bins use
  a relative `5e-4` envelope with a `2e-6` floor, which comfortably contains
  both the round-off and the anti-denormal contribution.

## 6. Mutation harness

`checks/mutations.sh` copies the production tree into a temporary directory
and applies one of two source edits per copy:

* `prewarp`: `LowPassTransform` prewarp `f = tan(pi fc)` replaced by the
  un-warped `f = pi fc`;
* `normalization`: the final gain normalization in `Cascade::setLayout`
  multiplied by an extra `0.97`.

Each broken copy must compile and must run a stereo PCM driver to finite,
non-empty output. A compile failure, crash, missing file or empty/non-finite
PCM is a hard failure and is never counted as a detection. Only when the build
is valid and the independent `mapping` assertions then fail does the script
report `DETECTED <name> build=0 numeric=1`; anything else exits non-zero. The
working tree is verified byte-identical before and after the run, since all
edits happen in temporary copies.
