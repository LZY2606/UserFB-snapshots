# Phase-locked pitch shifting (`AL_PITCH_SHIFTER_*_SOFT`)

OpenAL Soft's `AL_EFFECT_PITCH_SHIFTER` is a phase-vocoder: each analysis hop
(1024-sample Hann window, 128-sample hop) is FFT-analyzed, every bin gets its
own accumulated output phase, bins are remapped by the pitch ratio, and the
result is overlap-added. Because neighboring bins around one spectral peak
accumulate phase independently, the relative phases inside a peak drift after
shifting, which smears harmonic peak shapes and blurs percussive attacks.

This extension adds an optional **phase-lock** mode that keeps a peak's
internal phase relationships coherent while leaving the default behavior
byte-for-byte unchanged. It is a local OpenAL Soft extension: the parameters
are not part of the OpenAL standard and may change between releases.

## Parameters

| Enum | Value | Type | Range | Default |
|------|-------|------|-------|---------|
| `AL_PITCH_SHIFTER_PHASE_LOCK_SOFT` | `0x0003` | integer | 0 or 1 | 0 |
| `AL_PITCH_SHIFTER_TRANSIENT_THRESHOLD_SOFT` | `0x0004` | float | finite, [0, 2] | 0.5 |

All four scalar/vector setters and getters (`alEffecti/iv/f/fv`,
`alGetEffecti/iv/f/fv`) accept the matching type, and `alGetEnumValue`
resolves both names. A value out of range (including NaN or infinity for the
threshold) raises `AL_INVALID_VALUE`; querying or setting one parameter with
the other's type raises `AL_INVALID_ENUM`. A failed call never changes the
last valid value. Threshold `0.0` disables transient resets.

With phase lock off (the default) the effect behaves exactly as before,
including the coarse/fine tune quantization and the legacy magnitude
collisions.

## Kernel contract (`core/phase_lock.h`, `core/phase_lock.cpp`)

`al::PhaseLockBin` holds, in order, the bin `magnitude`, its wrapped input
`phase` in radians, and its instantaneous `frequency` expressed in FFT bins.

`al::PhaseLock1024::process(const PhaseLockBin *bins, float ratio, float
threshold, std::complex<float> *output)` takes 513 bins (a 1024-point real
FFT) and writes 513 complex synthesis bins. `ratio` is the existing
coarse/fine-derived, 16-bit quantized ratio in [0.5, 2]. All workspace is
fixed-size member or stack storage; `process` neither allocates nor frees
heap memory. A freshly constructed object and one that just ran `reset()`
behave identically.

Per analysis frame:

1. Only inner bins 1..511 are processed. Bin 0 (DC) and bin 512 (Nyquist)
   always synthesize to zero.
2. Let *M* be the maximum inner-bin magnitude. If *M* is zero, the output is
   zero and the peak trajectories are cleared.
3. A valid peak is a bin whose magnitude is at least *M*·10⁻⁴, is strictly
   greater than its left neighbor, and is at least as great as its right
   neighbor. If there are no valid peaks, the output is zero and the
   trajectories are cleared.
4. Every bin at or above the *M*·10⁻⁴ floor is assigned to its nearest valid
   peak; equal distances go to the lower-numbered peak. Bins below the floor
   contribute nothing.
5. Peaks are matched to the previous frame in ascending current-peak order,
   consuming each old peak at most once. A candidate is at most 3 bins away in
   bin index and at most 1.5 bins away in instantaneous frequency; the
   smallest frequency distance wins, with ties going to the lower old peak
   number. Peak numbers are per-frame labels, not persistent identities.
6. A matched peak continues its synthesis phase by
   `2π · 128/1024 · peak_frequency · ratio`. A newborn peak starts at the
   current input peak phase. All phases are wrapped to [-π, π).
7. Each assigned bin's synthesis phase is its peak's synthesis phase plus the
   bin's own input phase offset from that peak. Its polar contribution is
   accumulated (complex addition) onto target `floor(k·ratio + 0.5)`, keeping
   only targets 1..511. Colliding bins therefore add as complex numbers,
   replacing the legacy magnitude-sum/dominant-frequency rule; opposite
   phases can cancel.
8. **Transient detection runs before mapping and synthesis on the same
   frame.** Positive magnitude increments over all inner bins are summed and
   divided by the previous frame's total magnitude plus 10⁻¹². When a
   non-zero threshold is exceeded, every peak of the current frame restarts
   from its input peak phase. Previous-frame magnitudes are replaced every
   frame, including silent ones, so the first voiced frame after silence is
   always treated as a newborn peak (no matchable history).

The pitch shifter calls this kernel for every completed 1024/128 analysis
frame while phase lock is enabled, reusing the existing Hann window, FFT,
Bernsee instantaneous-frequency estimate, conjugate completion, inverse FFT
and output window. The legacy analysis and output phases keep advancing on
every hop regardless of mode, so switching back to the legacy spectrum is
seamless and the existing overlap-add tail keeps draining.

## Latency and mode switching

The output delay is unchanged: the STFT FIFO plus overlap-add add 1024
samples, exactly as in the legacy path; no extra delay is introduced.

Parameter changes are latched at the start of an analysis hop. Re-attaching
the effect to the same auxiliary effect slot therefore takes effect on the
next *completed* hop: an unfinished hop is never processed with two different
settings, even when `alcRenderSamplesSOFT` calls split that hop across
arbitrary chunk sizes. A mode switch resets only the phase-lock kernel's
history. The input FIFO, the output overlap accumulator and source playback
are never cleared or restarted.

## Verification

`tests/phase_lock_contract.cpp` is built and run by:

```sh
bash task_support/run.sh target
```

It registers three groups (`spectral`, `tracking`, `render`) and uses
`TASK_CHECK` for all assertions:

- **spectral / tracking** drive the production `al::PhaseLock1024` kernel and
  compare every bin against an independent double-precision reference:
  neighborhood phase coherence, complex collision/cancellation, floor
  assignment, one-to-one moving-peak inheritance, spectral-flux resets,
  silence rebirth and phase wrapping. Errors are measured as complex distance
  with an absolute 2·10⁻⁴ plus relative 2·10⁻⁵ tolerance, never magnitude
  alone. The reference never calls the production kernel.
- **render** runs the real effect on a 48 kHz stereo float loopback device
  with a mono source, a zero-gain direct low-pass filter, limiter and dither
  disabled, using non-integer-frequency harmonics, a glide and short pulses
  separated by silence. Wet PCM is predicted test-side by a direct DFT and
  frame-by-frame double reference using the same window/hop/scaling, with the
  FIFO latency and initial mixer gain accounted for. Whole-buffer renders and
  1/7/257/1025-frame chunked renders publish identical samples at the same
  absolute output frame once the initial gain ramp has settled, and a
  parameter switch is issued inside a non-hop boundary.
