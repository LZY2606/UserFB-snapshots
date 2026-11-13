# NES five-channel nonlinear mixing

The 2A03 APU normally drives each of its five channels (two pulse, triangle,
noise, DMC) through a separate linear amplitude path into its own
`Blip_Buffer`. Game_Music_Emu keeps that default behavior, and adds an
optional mode in which the five raw DAC levels are combined first, through
the nonlinear curves of the real hardware, and only the combined level is
band-limited once.

This document describes how that mode is implemented and what it does and
does not cover.

## API

* `Nes_Apu::set_nonlinear_output( Blip_Buffer* )` enables the mode and routes
  the joint mono signal to the supplied buffer. Passing `NULL` detaches the
  mixer and restores the regular per-channel `osc_output()` routing. It is
  intended to be called before `reset()`; switching modes during playback is
  not supported.
* `Nes_Apu::set_nonlinear_mute_mask( nes_time_t, int )` applies a mute mask
  in nonlinear mode. The low five bits select the channels (pulse 1, pulse 2,
  triangle, noise, DMC). Muting only zeroes a channel's contribution to the
  mixing formula; length counters, the noise shift register, envelopes and
  DMC fetches keep running, so unmuting reconnects the channel at its current
  state with no resynchronization.
* `Nsf_Emu::set_nonlinear_mixing( bool )` selects the mode for NSF playback.
  It must be set before `load()`. The choice is retained across `load()` and
  track restarts. With the ordinary `Stereo_Buffer`, the joint signal is sent
  to the center buffer so the left and right outputs are identical.

The default (linear) routing and its approximations are unchanged, including
the DMC `dac_table` pop correction used in that path.

## Mixing convention

The channels expose raw DAC integers: `p1`, `p2`, `t`, `n` in `0..15`, and
`d` in `0..127`.

The two pulse channels share one DAC curve. With `P = p1 + p2`:

```
pulse = P == 0 ? 0 : 95.88 / (8128 / P + 100)
```

Triangle, noise and DMC share the other curve. With
`S = t/8227 + n/12241 + d/22638`:

```
tnd = S == 0 ? 0 : 159.79 / (1/S + 100)
```

The mixed integer is

```
mixed = floor(16384 * (pulse + tnd) + 0.5)
```

computed with floating-point division. A `Blip_Synth<blip_good_quality,
16384>` turns the difference between successive mixed integers into the
band-limited signal; `Nes_Apu::volume()` and `treble_eq()` act on this
synthesizer. This is a fixed numerical convention, not a model of analog
filtering, so no additional circuit emulation is applied.

Adjusting a single gain, or applying a curve to each channel separately and
summing the results, is not equivalent: both curves are nonlinear, so the
joint level must be computed from the five raw levels together.

## Event merging on one timeline

The core correctness requirement is that all five channels share the same CPU
cycle timeline. Running one oscillator to the end of a segment and then
replaying its final level over another oscillator's earlier events would
misplace edges.

Each oscillator keeps its existing state machine (registers, envelope, sweep,
length and linear counters, frame-counter timing, PAL/NTSC periods). When the
mixer is attached, its `run()` reports every raw DAC level change to the
mixer as an event `(time, channel, level)` instead of writing to a
`Blip_Buffer`. The mixer:

1. collects the events produced by the channels during the segment;
2. stable-sorts them by CPU cycle, preserving the register-write call order
   for several edges that land on the same cycle;
3. folds all events of one cycle into the five current raw levels and
   evaluates the mixing formula once, emitting a single Blip delta for the
   change in the mixed integer.

Thus simultaneous changes from independent oscillators are merged before
resampling, and the formula is never applied to already-resampled PCM.
Events that do not change a channel's DAC level (for example the two triangle
valley phases that both read zero) are not emitted.

At `reset()` the post-reset raw levels are used as the initial mixing
reference (triangle via its existing `calc_amp()`, DMC via
`initial_dmc_dac`). No startup step is injected into an empty buffer.

## DMC dual-clock handling

The DMC runs on two related clocks. Its sample fetches and output unit are
advanced separately from the frame-counter-driven channels:

* `run_until()` may advance the DMC past the current frame so the CPU can
  account for upcoming memory reads (the NSF player uses this to insert wait
  states).
* `run_until_()` later advances the DMC again only up to the point the other
  four channels have reached.

The mixer therefore treats the DMC like the APU already does: it may have
queued events with timestamps beyond the current segment. `flush()` only
merges events up to the requested time and leaves later events queued, and
`end_frame()` shifts the timestamps of any leftover events to the new frame's
time origin. A DMC interval already consumed by `run_until()` is never run a
second time, so the fetch address sequence is identical whether or not the
early advancement interleaves with ordinary frame progress, and regardless of
how a frame is partitioned (whole frame, alternating 1/7/257-cycle segments,
and so on).

In nonlinear mode a `$4011` write sets the real 7-bit DAC value. The legacy
`dac_table` pop approximation is only applied on the old linear path and is
not fed into the nonlinear curve a second time.

## Muting and zero-volume regions

A muted channel keeps its full internal timing. Its raw level is simply
treated as zero inside the formula, so the DMC keeps fetching, the noise LFSR
keeps shifting, and counters keep counting while muted. The same property
holds for zero-volume intervals: the channel state machines continue to run,
and playback resumes from the live state rather than a stale one.

## NSF playback

`Nsf_Emu` wires the mode through the normal `Stereo_Buffer`: the joint signal
goes to the center buffer and is duplicated to both stereo outputs. The first
five logical voices' `mute_voices` bits map to the nonlinear mute mask.

Expansion chips (VRC6, VRC7, FDS, MMC5, Namco 163, FME-7) keep their
independent per-channel buffers. They are not folded into the nonlinear
formula, are not muted by the five-channel mask, and are not re-amplified by
the joint synthesizer. Restarting a track discards the previous track's
pending events and rebuilds the mixing reference.

## Scope and limitations

* Only the standard `Stereo_Buffer` (and mono `Blip_Buffer` use of
  `Nes_Apu` directly) is covered. Custom `Multi_Buffer` subclasses and the
  multi-channel export are not extended.
* The C API gains no new entry points.
* Hot switching of the mode while a track is playing is deliberately not
  implemented; configure it before `reset()` / `load()`.
* No analog-circuit filtering beyond the band-limiting `Blip_Synth` and the
  fixed nonlinear formula is modeled.
