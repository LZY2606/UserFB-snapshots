# Volume automation: frame time convention

SoLoud's volume automation (`fadeVolume`, `fadeGlobalVolume`, `oscillateVolume`,
`oscillateGlobalVolume`) is sampled on the **delivered output frame clock**, not
once per `mix()` call. This makes the exported transition shape depend only on
time: rendering a span in one block, in 16-frame blocks, or in blocks of 1, 7 or
257 frames produces the same envelope sample on every output frame.

## Time base

Let `R` be the output sample rate. A control call made between two `mix()`
calls takes effect starting with the *next* output frame, and its time origin
is the number of frames already delivered by that voice (or by the main
mixer, for global volume):

* `anchor = framesDelivered`
* output frame `k` after the call (`k` starts at 1) is evaluated at time
  `(anchor + k) / R`.

All four control functions use this convention.

## Linear fades

`fadeVolume(handle, to, T)` and `fadeGlobalVolume(to, T)` start from the value
in effect at the anchor (the last delivered value) and move linearly to `to`
over `T` seconds:

```
gain(k) = from + (to - from) * min(1, ((anchor + k) / R - anchor / R) / T)
```

Once `k/R >= T` the gain holds at `to`. A fade that ends after 64 frames
therefore reaches its target on frame 64 even if the surrounding `mix()` call
asked for 128 frames; the end point is not stretched to the block end.

`T <= 0` keeps the original immediate-set behaviour (it is routed to
`setVolume` / `setGlobalVolume`), as does `to == from`.

## Oscillators

`oscillateVolume(handle, from, to, T)` and `oscillateGlobalVolume(from, to, T)`
use the existing sine formula and period convention: `T` is the period in
seconds and the angular frequency is `2*pi/T`. The bounds are ordered so the
lower bound is the sine's minimum; the value at the anchor (before any output)
is the midpoint `(low + high) / 2`.

```
gain(k) = mid + amp * sin(2*pi * ((anchor + k) / R - anchor / R) / T)
```

The first framed value is the one at `1/R`, so peaks and troughs fall on the
frames implied by the time base and are not replaced by a straight line
between the first and last frame of a block.

## Getters and continuation

* Immediately after a control call the getters (`getVolume`,
  `getGlobalVolume`) return the value the automation will start from: the
  fade's current value, or the oscillator's midpoint.
* Calling a fade before any frame has been output continues from that value.
* Calling a fade during an active automation continues from the actual
  automatic value at the last delivered frame (`getVolume` / `getGlobalVolume`).
* After a `mix()` call the getters report the value at the last frame that was
  delivered by that call.

## Pausing

Automation on a voice or on a `Bus` voice freezes while that voice is paused:
the per-voice frame counter stops advancing, so the envelope resumes from the
same value after the voice is un-paused. The global volume clock keeps
advancing with output regardless of voice pause state.

## Placement in the gain chain

Voice automation is applied to the resampled source samples at the same gain
stage the old per-block volume occupied:

```
source -> per-stream filters -> voice volume automation -> panning/channel mix
       -> bus sum -> main filter -> global volume automation -> clip -> post-clip scaler
```

`Bus` and main-volume gains therefore multiply on the same output frame, and
a 512-frame resampling pre-read does not advance either clock ahead of the
delivered frames. Float (`mix`) and signed 16-bit (`mixSigned16`) output share
the same envelope. `setVolume` / `setGlobalVolume` with no automation still
apply the original ramped anti-click gain.
