# Volume automation frame-time convention

This build evaluates the volume automation used by `fadeVolume`,
`fadeGlobalVolume`, `oscillateVolume` and `oscillateGlobalVolume` in
**output-frame time**, so the rendered envelope is independent of how the
application splits its `mix()` calls.

## Time origin

Every control call happens between two `mix()` calls and starts from the audio
already delivered to the caller. If `t0` is that delivered output position and
the backend runs at `R` samples per second, then the `k`-th output frame after
the call (`k = 1, 2, ...`) is rendered at

```
t(k) = t0 + k / R
```

Frame `k = 0` is the last frame of the previous block, so the first new frame
uses `1/R`, never the block start.

## Fades

`fadeVolume(handle, to, T)` / `fadeGlobalVolume(to, T)` start from the value
the automation had at `t0` (the last delivered frame) and move linearly to `to`
over `T` seconds:

```
v(k) = from + (to - from) * (k / (T * R))
```

At and after `k >= T * R` the value holds `to`. A 64-frame fade that ends in
the middle of a 128-frame block is complete on frame 64 of that block, rather
than being stretched to the block end. Re-calling `fade` while another fade is
running starts from the value at `t0`, not from the original fade start.

A non-positive `T`, or a target equal to the current value, keeps the previous
behaviour: the volume is set immediately (one block of anti-click ramp remains
for `setVolume`/`setGlobalVolume`).

## Oscillation

`oscillateVolume(handle, lo, hi, T)` / `oscillateGlobalVolume(lo, hi, T)`
accept `lo <= hi` (a reversed pair is swapped) and use the existing sine shape
with period `T`:

```
v(k) = mid + amp * sin(2 * pi * k / (T * R)),  mid = (lo + hi) / 2, amp = (hi - lo) / 2
```

The first frame is evaluated at `1/R`, so it starts at the midpoint zero
crossing and the peaks and troughs inside a block are rendered exactly; a
block only samples the sine at frame times and never replaces it with an
endpoint-to-endpoint ramp. Before any frame has been rendered the value
reported by `getVolume`/`getGlobalVolume` is that midpoint, and a `fade` issued
before the first frame continues from it.

## Getters and clocks

`getVolume`/`getGlobalVolume` report the automation value of the **last
delivered output frame** in the most recent `mix()` (for a block of `N` frames
that is frame `N`, at `t0 + N/R`).

A voice or bus has its own sample clock (`mStreamTime`): while it is paused its
automation freezes, and when it resumes it continues from where it stopped. The
main/global volume clock always advances with output, even when every voice is
paused.

## Gain application

Automation multiplies at the same gain stage the fixed volume used before:

* a voice's `mVolumeFader` scales its source samples before panning/channel
  expansion (after any per-source filters and before the global filter);
* the bus instance is itself a voice, so a bus gain is applied at that stage
  and sums with its fixed child voices;
* the global volume is applied by the clip stage, on the same output frame as
  the bus gain. The 512-frame source prefetch never advances either clock.

Clipping, round-off and the post-clip scaler keep their original order after
these gains; neither `mix()` nor the backend is re-entered per frame.

## Where this lives

`src/core/soloud_fadertime.cpp` holds the per-frame evaluation helpers;
`mix_internal` / `panAndExpand` / `clip_internal` in `src/core/soloud.cpp`
apply the per-frame gains. `tests/task_fade.cpp` checks the envelope, clock and
routing behaviour with the null driver.
