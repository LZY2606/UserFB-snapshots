// beat_timeline.ck
//
// Absolute-beat timeline for the embedded regression host.
// Start at 60 BPM, ramp linearly to 120 BPM over two beats, and emit
// one-sample Step impulses at beat 2 and beat 3.  The host checks the
// complete float32 PCM, including silence everywhere else.

BeatClock c;
c.bpm(60.0);
c.ramp(120.0, 2.0);

Step s => dac;
0.0 => s.next;

c.at(2.0) => now;
0.25 => s.next;
1::samp => now;
0.0 => s.next;

c.at(3.0) => now;
-0.5 => s.next;
1::samp => now;
0.0 => s.next;

// Keep the shred alive long enough for the offline host to render/check.
10::second => now;
