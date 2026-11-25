# BeatClock

`BeatClock` is ChucK's built-in beat-based timeline.  Unlike converting a beat
value to fixed seconds once, every deadline is recomputed from an absolute
tempo-curve anchor.  Changing BPM or replacing a ramp therefore moves pending
beat events without restarting their wait duration.

## ChucK interface

```ck
BeatClock c;              // beat 0 at the current VM time; 120 BPM

float b = c.beat();       // current absolute beat
float q = c.bpm();        // current instantaneous BPM

c.bpm(96.0);              // constant BPM, beat position stays continuous
c.ramp(144.0, 4.0);       // BPM vs. beat: current BPM -> 144 over four beats

Event e = c.at(8.0);      // one-shot event at absolute beat 8
e => now;
```

The method signature is:

- `float beat()`
- `float bpm()`
- `float bpm(float value)`
- `void ramp(float targetBpm, float beats)`
- `Event @ at(float absoluteBeat)`

BPM and ramp lengths must be positive and finite.  The target beat passed to
`at()` must be non-negative and finite.  Invalid arguments use ChucK's native
`InvalidArgument` exception path; the previous curve and pending waits are left
unchanged.

## Time and rounding

Time is measured on the normal sample timeline at the VM sample rate.  For a
constant tempo `q`, the sample duration of `db` beats is:

```text
samples = 60 * sampleRate * db / q
```

A ramp is linear in **beats**, not wall-clock seconds.  If BPM changes from
`q0` to `q1` over `L` beats, the exact integral of `60 / bpm(beat)` is used.
All formulas are evaluated from the newest absolute anchor `(sample, beat,
currentBpm)`; rounded segment durations are never accumulated.

Beat deadlines are rounded to the nearest integer sample frame, with exact
halves rounded upward.  At 48 kHz, 60 BPM ramping to 120 BPM over two beats
gives:

- beat 2: frame `66542`
- beat 3: frame `90542`

## Scheduling and ordering

`at()` returns an ordinary ChucK `Event` backed by BeatClock scheduling state.
Many shreds may wait on the same event.  Reaching its frame broadcasts it once
and latches it as complete; waiting again continues immediately.

Beat deadlines participate in the VM's next-wake calculation and cap adaptive
block lengths so the VM cannot process across a deadline.  There is no
per-sample polling, extra thread, or one-timer-shred-per-waiter.

At one frame:

1. ordinary timed shreds retain their existing ready-queue order;
2. due beat-event waiters are appended in first-actual-wait order of their
   beat events;
3. waiters on one Event retain that Event's internal wait order.

A waiting beat event owns its clock curve, so dropping the ChucK `BeatClock`
variable does not invalidate a pending wait.  Removing a waiting shred removes
its wait reference.  VM shutdown detaches VM-keyed scheduler state.

See `examples/beat_timeline.ck` for the offline PCM example.
