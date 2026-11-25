# BeatClock

`BeatClock` is a built-in ChucK class for beat-position scheduling on the
audio sample timeline.  A clock is anchored when it is constructed: the VM's
current sample time is beat `0`, and the initial tempo is `120` BPM.

## Interface

```chuck
BeatClock c;

float b = c.beat();       // current continuous absolute beat
float t = c.bpm();        // current tempo, including inside a ramp
c.bpm(132.0);             // hold 132 BPM; preserve the current beat
c.ramp(160.0, 4.0);       // BPM linear in beat position over four beats
Event e = c.at(8.0);      // one-shot event at absolute beat 8
e => now;                 // block until that beat's sample frame
```

`bpm(value)` keeps the current beat/sample position continuous and cancels any
active ramp.  `ramp(targetBpm, beats)` starts from the real BPM at the call
position, changes BPM linearly as a function of beat position, reaches the
target after `beats` beats, and then holds the target.  Calling either setter
again in the middle of a ramp creates a new absolute anchor at the real
current sample and beat position; waiting events are recomputed from the new
curve rather than storing an old sample deadline or restarting a beat count.

`bpm(value)` requires a positive finite value.  `ramp(targetBpm, beats)`
requires both arguments to be positive and finite.  `at(absoluteBeat)`
requires a non-negative finite beat.  Invalid arguments use ChucK's native
`InvalidArgument` exception path and do not modify the current curve or
existing waits.

## Time conversion

For a segment anchored at absolute sample `s0`, beat `b0`, tempo `v0`, and a
ramp to `v1` over `L` beats:

\[
v(b)=v_0+(v_1-v_0)\frac{b-b_0}{L}
\]

and, for `v1 != v0`,

\[
s(b)=s_0+\frac{60\,f_s L}{v_1-v_0}
\ln\!\left(\frac{v(b)}{v_0}\right).
\]

After the ramp, time continues at `v1` BPM.  Conversion always evaluates from
the segment's exact absolute anchor; rounded frame durations are never
accumulated between segments.  Event deadlines are the nearest integer sample
frame, with exact halves rounded upward.

At 48 kHz, a 60-to-120 BPM ramp over two beats places beat 2 at frame
`66542`; beat 3 is one 120-BPM beat later at frame `90542`.

## Scheduling and lifetime

`at()` returns an ordinary one-shot ChucK `Event`.  Several shreds may wait on
the same returned object and are broadcast in the Event's wait order.  If the
target beat has already been reached when `event => now` executes, the wait
continues immediately; the event is latched and repeated waits also continue.

Beat deadlines participate in the VM's normal next-wake calculation and the
adaptive block-size cap.  The implementation does not poll samples, create an
external thread, or create a timer shred for each waiter.  At one sample
frame, beat events are broadcast in the order in which each event received its
first actual wait.  Their awakened shreds are appended to the existing
equal-time ready queue, preserving ordinary sample/duration wait order.

A waiting beat Event owns its clock's timing state, so dropping the ChucK
`BeatClock` variable does not invalidate an outstanding wait.  Removing a
waiting shred removes its Event wait reference through the normal shreduler
path.  Beat bookkeeping creates no shred and therefore does not by itself keep
the VM alive.  Clock/event side tables are isolated by VM and are discarded
on VM shutdown.

See `examples/beat_timeline.ck` for the canonical two-pulse PCM regression.
