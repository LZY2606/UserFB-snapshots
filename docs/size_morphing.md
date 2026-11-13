# Continuous room-size modulation

MVerb's legacy SIZE parameter reallocates the meaning of the tank delay lines
on every change and **clears** the four tank allpasses and four static
delays. Automating SIZE while a sound is still decaying therefore destroys
the reflections already travelling through the room. The continuous-size
mode added to the C++ core (`MVerb.h`) changes the physical propagation
lengths of the same dual-tank network instead, so in-flight sound survives.

## Interface

Both methods are members of `MVerb<T>`:

```cpp
bool enableSizeMorph(bool enabled);
bool morphSize(T normalized, unsigned frames);
```

### `enableSizeMorph`

- Continuous size morphing is **off by default**.
- It can only be toggled after a `reset()` (which includes construction and
  `setSampleRate`) and **before the first non-empty `process` call**. A
  zero-length `process` call does not consume this window. Toggling while
  audio has run returns `false` and changes nothing.
- When enabled, the mode starts at the size currently held by SIZE.
- `reset()` clears all histories and any unfinished trajectory but retains
  the enabled/disabled state, restarting from the most recent target size.

### `morphSize`

- `normalized` must be finite and within `[0, 1]`; the actual room size used
  by the network is `0.05 + 0.95 * normalized`, identical to the SIZE
  parameter mapping. Infinity, NaN and out-of-range values return `false`
  and leave both the audio and the modulation progress untouched.
- After a command is accepted, the size moves in equal increments from the
  next output frame, reaching the target after `frames` output samples.
  `frames == 0` applies the target immediately on the next frame.
- If a new command arrives before the previous ramp has finished, it restarts
  from the size the next frame would have used — never from the starting
  point of the earlier command.
- Zero-length `process` calls do not advance the trajectory.
- While the mode is enabled, the legacy SIZE setter
  (`setParameter(SIZE, ...`) behaves exactly like `morphSize(value, 0)` and
  never clears the tanks. With the mode disabled it keeps its original
  clearing behaviour, so existing users are unaffected.
- Calling `morphSize` while the mode is disabled returns `false`.

## What changes inside the network

Only the four tank allpasses and four static delays change behaviour; input
diffusion, predelay, early reflections, the cross-feedback between the two
tanks and the signs of the left/right output taps are untouched.

- The per-second base lengths and tap constants from `reset()` are retained.
  Every frame each line computes its floating-point main length
  `L = seconds * sampleRate * currentSize` and tap offsets
  `q = tapSeconds * sampleRate * currentSize`, without rounding first.
- Each line stores its history ordered by actual write time in a fixed-size
  ring buffer whose capacity is bounded by the sample rate and the maximum
  size (no whole input is recorded, and no memory is allocated or released
  while processing or handling commands). Reads of negative time, or of
  samples older than the buffer, return zero; fractional positions use
  linear interpolation. Old samples are neither moved nor cleared when the
  size changes, and each line is written exactly once per frame.
- The main output is read **before** the write. The integer lines return
  `x[n - L]` there, i.e. age `L - 1` relative to the most recently written
  sample; the fractional lines use the same age convention.
- The allpass keeps the original structure `y = d - g*x` and stores
  `x + g*y`.
- Output taps are observed **after** the write and advance, matching the
  legacy `GetIndex` timing, and read history at age `L - q - 1`. The tap
  offset `q` is never used as a delay on its own, and every tap reads the
  same frame's size as the main length of that line.
- The previous-frame cross-feedback values and the damping filter histories
  continue normally across size changes.

Before the first non-empty `process` call in morph mode, the legacy block
smoothers are pinned to their current target values so old intra-block
parameter ramps cannot perturb size comparisons. Other parameters are not
required to be automated alongside SIZE.

Processing supports fully in-place channel buffers. The first supported
configuration is 48 kHz over the full size range, where every tank line's
minimum main length is greater than one and every tap age stays positive.

## Verification

`task_support/target.cpp` contains three groups run by
`task_support/target.sh`:

- `delay` — fractional delay, allpass and post-write taps checked against an
  independent time-ordered history.
- `tank` — the public dual-tank connection re-derived independently,
  including asymmetric input, a decaying tail and amplitude scaling over the
  full size range.
- `automation` — mid-trajectory redirects, immediate switches, invalid
  commands, zero-length blocks, in-place processing and 1/7/113-frame
  chunking.

The double-precision tolerance is `1e-8 + 1e-7 * |reference|`; the
references never call the new delay implementation.
