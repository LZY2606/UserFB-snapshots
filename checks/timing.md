# Batch digital advance: scope, approximations, and skip evidence

`SID::clock(cycle_count delta_t)` now advances every digital part of the
chip exactly as `delta_t` consecutive `SID::clock()` calls, using
event-driven fast forward instead of the old approximate batch paths
(`WaveformGenerator::clock(delta_t)`,
`WaveformGenerator::set_waveform_output(delta_t)`,
`EnvelopeGenerator::clock(delta_t)` — all removed/replaced). The single
cycle entry `SID::clock()` is untouched and serves as the reference.

## Supported scope (bit exact with per-cycle clocking)

For both MOS6581 and MOS8580, after any `clock(N)` the following match
`N` × `clock()` exactly, including the hidden pipeline state (verified by
continuing with 64 single cycles and by toggling GATE/TEST afterwards):

* oscillator accumulators, including hard sync (all oscillators advance
  in parallel before sync is evaluated; a sync source that is itself
  synced on the same cycle does not reset its destination) and ring
  modulation MSB substitution;
* the noise shift register, its 2-cycle shift pipeline (a pending shift
  is never dropped across entries), and the TEST-bit shift register
  reset timers with their bitfade re-arming;
* the pulse comparator pipeline (`pulse_output`), forced high under TEST
  by `WaveformGenerator::clock()` every cycle;
* the floating DAC timer and bitfade chain when the waveform register
  is cleared;
* the ADSR envelope machines: rate counter (with the 0x7fff wrap of the
  ADSR delay bug when the rate period is lowered below the counter),
  exponential counter and period, envelope counter, state, hold-zero,
  and the envelope/exponential/state pipelines;
* readable digital registers OSC3/ENV3 (including the 8580
  triangle/sawtooth half-cycle output delay) and the data bus value;
* the MOS8580 one-cycle register write pipeline: the first cycle of a
  batch never sees the new register values.

Combined waveforms that write back into the accumulator (6581, saw plus
any of tri/pulse/noise) or into the noise shift register (noise combined
with anything) are stepped cycle by cycle, since their per-cycle analog
short-circuit effects are not safely skippable. Under TEST these
writebacks are inert (the accumulator stays zero, the shift register
write is blocked), so TEST regions still skip.

## What stays approximate (analog)

* The internal filter (`Filter::clock(delta_t, ...)`) and the external
  filter (`ExternalFilter::clock(delta_t, ...)`) keep the original batch
  approximation, clocked once per `clock(delta_t)` call with the final
  voice outputs. Analog PCM is therefore not bit identical between
  per-cycle and batch advance, by design.
* The data bus ttl counter is folded (`ttl -= delta_t`, clamped); its
  internal value may differ from per-cycle clocking, but the readable
  bus value and the survival time of a rewritten value are identical.
* Sampler and waveform measurement tables are unchanged.
  `SAMPLE_INTERPOLATE`/`SAMPLE_RESAMPLE`/`SAMPLE_RESAMPLE_FASTMEM` still
  clock per cycle as before; `SAMPLE_FAST` now rides the exact batch
  digital engine through `clock(delta_t)`.

## How skipping works

* Envelopes (`EnvelopeGenerator::clock(delta_t)`): the machine is a
  self-contained counter loop. `quiet_cycles()` computes the exact cycle
  of the next event (state-pipeline action, envelope/exponential
  pipeline firing, rate-counter reset with an effect) and
  `advance_quiet()` folds the quiet span with counter/phase arithmetic:
  the rate counter loop (period `rate_period + 1`, including the
  0x7fff wrap), no-effect resets, exponential counter increments, and
  the sustain-level steady state (exponential counter sawtooth and
  pipeline re-arming reconstructed at the landing cycle). Event cycles
  are processed with the regular single-cycle `clock()`.
* Oscillators (`SID::clock(delta_t)` event loop): each voice computes
  `quiet_cycles()` — cycles until accumulator bit 19 rises (arms the
  noise shift pipeline), the shift pipeline expires, the MSB rises while
  the sync destination listens, the TEST reset timer expires, or the
  floating DAC timer expires. Quiet spans advance the accumulator
  linearly (`acc += q*freq mod 2^24`) and recompute waveform outputs
  from the final and previous-cycle accumulator; event cycles run the
  normal clock/synchronize/output sequence for all voices in parallel.
* Nothing is cached across calls and no future state is precomputed;
  memory use is O(1) regardless of `delta_t`, and digital precision is
  never reduced (no output-rate decimation).

## Instrumentation and skip evidence

Counters live where the work happens: `SID::debug_cycle_updates` counts
whole-chip cycle-by-cycle digital updates (the combined-waveform
fallback), `SID::debug_event_updates` counts O(1) event-cycle
processings (envelope events in `EnvelopeGenerator::clock(delta_t)`,
oscillator event cycles and the 8580 write-pipeline cycle in
`SID::clock(delta_t)`). Quiet spans are not counted.

Measured with `checks/target.cpp skip` (1,000,000 cycles per scenario,
state verified equal to 1,000,000 reference `clock()` calls):

| scenario                                   | cycle updates | events |
|--------------------------------------------|---------------|--------|
| reset, freq=0, gate off (6581)             | 0             | 1530   |
| reset, freq=0, gate off (8580)             | 0             | 1530   |
| low-freq saw + sustaining envelope (6581)  | 0             | 1679   |
| low-freq saw + sustaining envelope (8580)  | 0             | 1957   |
| low-freq noise + sustaining envelope (6581)| 0             | 1679   |
| low-freq noise + sustaining envelope (8580)| 0             | 1957   |

The hard limit is 64 cycle-by-cycle updates for the reset/freq=0/gate-off
case; the measured value is 0. The events are the one-time envelope decay
from the power-up value 0xaa to zero (about 170 counter steps with their
pipeline cycles) plus noise shift events; after the envelope settles the
counters stop moving, so the skip is not a reset special case — the
saw/noise scenarios keep a running oscillator (bit-19 pipeline events,
noise shifts) and a sustaining envelope (exponential counter cycling) and
still perform zero cycle-by-cycle updates.
