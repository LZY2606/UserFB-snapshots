# Nsf_Pump: budget-driven, pausable NSF playback

`gme/Nsf_Pump.h` adds a C++ playback entry point that lets a cooperative
audio task scheduler reclaim control in the middle of an internal audio
frame. It uses the same 6502 core (`Nes_Cpu`), APU (`Nes_Apu`), NSF frame
advance logic (`Nsf_Emu`), and `Stereo_Buffer`/`Blip_Buffer` resampling
path as the classic synchronous `Nsf_Emu` interface — no threads, no
second emulation core, and no "run a whole `play()` then hand out the
sound in pieces" batching.

## Lifecycle

```cpp
Nsf_Pump pump;                      // default-constructed, empty
blargg_err_t err = pump.begin( data, size, 44100, 0 );
Nsf_Pump::Result r;
while ( ... )
    err = pump.pump( out, capacity_frames, budget, &r );
pump.reset();                       // also safe to call repeatedly
```

* `begin( data, size, rate, track )` copies the complete NSF1 file
  (NTSC or PAL, no expansion audio chips), then performs only the usual
  load and track-start setup. It does **not** execute the NSF's INIT or
  PLAY routines; those run later, inside `pump()`. `rate` must be 44100
  or 48000 and `track` is a zero-based, in-range track number. Other NSF
  versions, expansion-audio files, and invalid parameters return a
  non-null error. A successful `begin` replaces any previous playback
  and clears the progress counters; a failed one leaves the previous
  playback untouched. The configuration is fixed: tempo 1.0, default
  equalizer, no fade-out, and automatic silence trimming disabled.
* `reset()` stops playback and clears the counters. It is safe to call
  any number of times, including before the first `begin`.
* `pump()` before any successful `begin()` returns an error.
* `Nsf_Pump` is default-constructible and destructible, but not
  copyable: it owns the single copy of the input, the live CPU/APU
  state, and the fixed-size playback buffers. Memory use does not grow
  with playback time.

## Execution budget

`pump( out, capacity_frames, budget, &result )` stops and returns as
soon as either independent limit is reached:

* **1 budget unit** per real 6502 instruction completed, and
* **1 budget unit** per PCM frame (one interleaved left/right sample
  pair) delivered to the caller.

`result->work` always equals `result->instructions + result->frames`
and never exceeds `budget`. `capacity_frames` and `budget` are
independent upper bounds; `pump` writes only the frames it actually
returns and never pads with silence.

Only genuine instruction completion counts. The internal return
sentinel used to end INIT/PLAY calls, idle time skips, and the fixed
housekeeping transitions (scheduling PLAY, restoring the saved caller
context) are not instructions and cost nothing. Cycle counts are never
substituted for instruction counts: the budget is enforced inside the
6502 core's dispatch loop, so a pause happens exactly at an instruction
boundary. Even `budget = 1` advances a long-running INIT or PLAY by one
instruction. When the budget runs out mid-frame, the CPU registers,
`next_play`, the saved caller context, and APU time all remain parked
inside the same unfinished internal frame; `end_frame` is neither
called early nor repeated, and no smaller substitute frame is
emulated. The next `pump()` resumes at the next instruction — earlier
register writes, DMC fetches, and bank switches are not replayed, and
no CPU-state checkpoints are kept.

Internal audio frame boundaries are the original ones (the buffer
length used by `Classic_Emu`). Only after a whole internal frame has
been emulated are samples produced through the original
`Stereo_Buffer`/`Blip_Buffer` path; the integrator state, fractional
phase, and any undelivered remainder persist between calls.

Edge cases:

* `capacity_frames == 0` or `budget == 0`: returns success with an
  all-zero `Result` and makes no progress at all.
* `capacity_frames > 0` requires a non-null `out`; `result` must always
  be non-null. Negative capacity, null pointers, and pumping before
  `begin()` all return errors without touching playback state.
* While a track is playing and both limits are positive, every
  successful call reports `work > 0`.

## Clocks and progress

`progress()` is side-effect free and reports cumulative counters since
the last successful `begin()` or `reset()`:

* `instructions` — real 6502 instructions completed, counted where the
  CPU actually retires them.
* `delivered_frames` — stereo frames handed to the caller; grows only
  when `pump()` delivers.
* `emulated_clocks` — total NES clocks actually executed or advanced
  by idle skips. It is monotonic and does not reset when the internal
  time base is rebased between frames.

## Output equivalence

For supported inputs, the concatenated PCM delivered by any sequence of
`pump()` calls — any budget pattern (including 1), any capacities
(1/7/257, ...), across PLAY and internal frame boundaries, and with
multiple `Nsf_Pump` instances interleaved — is sample-for-sample
identical to an independent `Nsf_Emu` playing the same track
sequentially with the same fixed configuration, and the DMC fetch
sequence is unchanged. The classic synchronous interface
(`Nsf_Emu::play()` and friends) runs on the same instruction-execution
and frame-advance core, so its default output is unchanged as well.
