# High-Order B-Format Offline Regression

This directory contains an offline, device-free regression for the Ambisonics
field rendering path. It drives the public AL/ALC loopback interface only; the
player UI is not extended, no audio hardware or recording files are used, and
the reference implementation is independent of the library internals.

## What is tested

`ambi_contract` opens a 48 kHz, 3D B-Format, float32 loopback device whose
output is fixed to ACN layout with N3D scaling. It synthesizes signals made of
a low-amplitude sine bed plus several signed directional fields, renders them,
and compares the returned PCM sample by sample against an independent
double-precision spherical-harmonic reference (`ambi_reference.*`).

Four groups are printed, each with a positive case count and the worst
absolute error, and the program exits non-zero on any failed assertion:

```
PASS normalization cases=7 max_error=...
PASS rotation cases=4 max_error=...
PASS live-update cases=2 max_error=...
PASS queue cases=2 max_error=...
```

### 1. Input normalization / format conversion

The same physical field is encoded as first-order FuMa, first/second/third
order ACN/SN3D and ACN/N3D. Because the device output is always ACN/N3D, the
three representations of one field must render to identical PCM; the group
also pins the amplitude, sign and phase of every channel.

### 2. Rotation into the audio

Sources and listeners get combined yaw, pitch and roll with non-commuting
angles. The expected field is never produced with a rotation matrix: each
direction is transformed geometrically, then the spherical harmonics are
re-evaluated on the transformed direction. Both head-relative sources
(listener orientation ignored) and world sources (listener orientation
applied) are covered, so the two code paths are distinguished.

### 3. Live orientation updates over a queue

A continuous signal is split into three unequal buffers queued on one source.
Source and listener orientation change at frames that are not buffer
boundaries, while rendering continues across the queue boundaries. Stable
windows (at least 2048 frames after each play/update and away from the next
transition) are compared against the geometric expectation, and a static
single-buffer render of the same signal acts as a control. Processed-buffer
counts and the final `AL_STOPPED` state are checked too.

### 4. Queue handoff

Phase continuity across the three buffer boundaries, the ordered
`AL_BUFFERS_PROCESSED` counters and the final stopped state are verified,
again against the geometric sample expectation.

## Coordinate conventions

OpenAL world axes are `+X` right, `+Y` up, `-Z` forward, with the default
orientation `at=(0,0,-1)`, `up=(0,1,0)`. The Ambisonics frame used by the
reference is `+X` forward, `+Y` left, `+Z` up, so

```
Ambisonics Y = -OpenAL X,  Ambisonics Z = OpenAL Y,  Ambisonics X = -OpenAL Z
```

ACN indexes degree `n` and order `m` (`-n <= m <= n`) as `n*(n+1)+m`. The
real harmonics use the N3D orthonormal scaling:

```
N_n^m = sqrt((2n+1) * (n-|m|)! / (n+|m|)!)
m > 0 -> sqrt(2) N_n^m cos(m phi)
m < 0 -> sqrt(2) N_n^|m| sin(|m| phi)
m = 0 -> N_n^0 P_n
```

with the associated Legendre functions built by the standard recurrence
**without** the Condon-Shortley phase, matching OpenAL Soft's ambisonic basis
(`Y_0^0 = 1`). SN3D is N3D divided by `sqrt(2n+1)`; first-order FuMa uses the
`1/sqrt(2)` W term, SN3D-scaled XYZ and the `W,X,Y,Z` channel permutation.

The reference first validates itself against the constant term and the three
axial first-order fields (`REFERENCE_SELFTEST`) before any library output is
trusted.

## Why the stable windows are 2048 frames

The mixer applies a short gain ramp whenever a source (re)starts or its
parameters change, so samples around a play/update instant are transient by
design. The test never asserts on the ramp shape itself; it leaves 2048 frames
after every play/update and keeps each comparison window clear of the next
transition and of the queue boundaries, then checks only the steady region.
Output buffers are pre-filled with a sentinel value so any unwritten region is
caught, and every sample is checked for finiteness. Signal amplitudes stay
well below clipping, the distance model is disabled, near-field control is
inert at the origin (no NFC reference distance is configured), pitch and
sample rate are unchanged, and the limiter and dither are disabled.

The absolute per-sample tolerance is `2e-4`; on the current implementation the
observed worst error is several orders of magnitude smaller.

## Running

```sh
# Build (offline loopback only needs the static library and this target)
cmake --build build --target ambi_contract

# Direct
ALSOFT_CONF=$PWD/baseline/alsoft.conf ./build/tests/ambi_contract

# Via CTest (registered as the ambi_contract test)
ctest --test-dir build -R '^ambi_contract$'
```

## Mutation runner

`run_ambi_contract.sh` builds isolated source/build copies under
`build/ambi-regression`: one unmodified baseline and three single-point
mutations, each in its own copy so the source tree is never touched.

- `scale`    - ignores the per-input-channel normalization scale in the
               B-Format panner (breaks FuMa/SN3D conversion);
- `listener` - skips the listener-frame transform of world-source orientations
               (turning the listener no longer moves a world field);
- `hoa`      - caps the high-order rotator at first order.

Every copy must configure, compile and actually render PCM. A mutation counts
as detected only when the `ambi_contract` binary exits `1` on a numerical
assertion; compile failures, missing binaries, timeouts or any other exit code
are reported as failures rather than detections. Build and run logs (including
the first mismatched frame/channel/expected/actual value) are kept in
`build/ambi-regression/logs`. On success the script prints
`DETECTED scale`, `DETECTED listener`, `DETECTED hoa` and
`ALL_MUTATIONS_DETECTED`.

## The rotation bug this regression caught

The adapted Ivanic-Ruedenberg / Politis recursion in `alc/alu.cpp`
(`AmbiRotator` and the `rotator` helpers) had its first-band and previous-band
matrix indices transposed relative to the source algorithm, used a folded
`m == 0` coefficient and routed the zero-order row through the generic `V()`
combination. For any rotation away from a pure yaw the generated second/third
order bands were not orthogonal (even `R(beta) R(-beta) != I`) and the band
diagonal did not equal the Wigner small-d value. The fix restores the exact
Table I coefficients, the `R_1(i+2, .)` / `R_lm1(a+l, b+l)` indexing, the
dedicated `m == 0` branch and the matching loop/coefficient ordering. The
geometric reference (cross-checked by spherical quadrature and the library's
own closed-form polynomials during development) now matches to roughly `1e-7`.
