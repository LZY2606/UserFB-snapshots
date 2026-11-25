// beat_timeline.ck
//
// BeatClock timeline for the offline regression host.
// Start at 60 BPM, ramp linearly (as a function of beat) to 120 BPM over
// two beats.  The ramp formula places beat 2 at exact sample frame 66542;
// beat 3, one constant 120-BPM beat later, is at frame 90542.

BeatClock c;
Step pulse => dac;

0 => pulse.next;

c.bpm( 60.0 );
c.ramp( 120.0, 2.0 );

// Single-sample +0.25 pulse at beat 2.
c.at( 2.0 ) => now;
0.25 => pulse.next;
1::samp => now;
0 => pulse.next;

// Single-sample -0.5 pulse at beat 3.
c.at( 3.0 ) => now;
-0.5 => pulse.next;
1::samp => now;
0 => pulse.next;
