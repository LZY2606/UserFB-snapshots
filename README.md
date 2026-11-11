
sndfilter
=========

Algorithms for sound filters, like reverb, dynamic range compression, lowpass, highpass, notch, etc.

It's easy to find countless math equations on sound filters, but a bit harder to find simple source
code.  This library is my attempt at cleaning up and presenting the math-heavy filter algorithms for
the programming community.

Please note that I favored simple code over fast code.  Hopefully it's made it more understandable.

(0BSD License)

Build Instructions
------------------

The `./build` script is a simple bash script that compiles the source files using `clang`.  It's
dirt simple, [I promise](https://github.com/velipso/sndfilter/blob/master/build).

Simply run `./build` and the executable should be `./tgt/sndfilter`.

### C++ Support

This project is pure C, but I've left PRs open for those who want C++ support.  Check them out, they
might save you some time:

* [C++ headers](https://github.com/velipso/sndfilter/pull/18)
* [Meson build system support](https://github.com/velipso/sndfilter/pull/19)

Filters
-------

* [Reverb](https://en.wikipedia.org/wiki/Reverberation) (Algorithmic)
* [Compressor](https://en.wikipedia.org/wiki/Dynamic_range_compression)
* [Low-Pass](https://en.wikipedia.org/wiki/Low-pass_filter) (Cutoff, Resonance)
* [High-Pass](https://en.wikipedia.org/wiki/High-pass_filter) (Cutoff, Resonance)
* [Band-Pass](https://en.wikipedia.org/wiki/Band-pass_filter) (Frequency, Q)
* [Notch/Band-stop](https://en.wikipedia.org/wiki/Band-stop_filter) (Frequency, Q)
* [Peaking](https://en.wikipedia.org/wiki/Equalization_(audio)) (Frequency, Q, Gain)
* [All-Pass](https://en.wikipedia.org/wiki/All-pass_filter) (Frequency, Q)
* [Low Shelf](http://www.audiorecording.me/what-is-a-low-shelf-and-high-shelf-filter-in-parametric-equalization.html) (Frequency, Q, Gain)
* [High Shelf](http://www.audiorecording.me/what-is-a-low-shelf-and-high-shelf-filter-in-parametric-equalization.html) (Frequency, Q, Gain)

Compressor Usage
----------------

The compressor is a streaming, stateful filter. Initialize an `sf_compressor_state_st` once with
`sf_defaultcomp`, `sf_simplecomp`, or `sf_advancecomp`, then feed it frames with
`sf_compressor_process`.

`sf_compressor_process` accepts any non-negative number of frames per call — 1, 17, 33, or a full
block alike. There is no alignment requirement on the chunk size. From initialization onwards,
every `SF_COMPRESSOR_SPU` (32) consecutive frames form one control period whose envelope
parameters stay fixed; call boundaries never restart a period, so feeding one stream in different
chunk sizes produces bit-identical output. A size of zero is legal (the pointers may be `NULL`)
and advances no state, and `input` may alias `output`. No memory is allocated while processing.

The compressor delays its output by `sf_compressor_latency()` frames (the predelay ring buffer's
`delaybufsize - 1`). Once the input stream ends, pull those frames back out with
`sf_compressor_drain`, which is equivalent to feeding that many zero frames into
`sf_compressor_process`:

```c
sf_compressor_state_st comp;
sf_simplecomp(&comp, 48000, 5, -24, 30, 12, 0.003f, 0.250f);

// process the stream one callback at a time, whatever size the callback delivers
for (each input chunk)
    sf_compressor_process(&comp, chunk_size, input, output);

// emit the delayed tail, split across calls if the output capacity is small
int latency = sf_compressor_latency(&comp);
int emitted = 0;
while (emitted < latency)
    emitted += sf_compressor_drain(&comp, latency - emitted, tail + emitted);
```

A capacity of zero writes nothing and does not end the stream, so it is safe to poll. The first
call with a positive capacity finalizes the instance: afterwards `sf_compressor_process` leaves
the output and state untouched until one of the initialization functions is called again to start
a new stream. A stream that never processed a frame has no tail, and repeated calls after the tail
is exhausted return zero. No extra silence is emitted to align control periods.

Implementation
--------------

The [reverb.c](https://github.com/velipso/sndfilter/blob/master/src/reverb.c),
[compressor.c](https://github.com/velipso/sndfilter/blob/master/src/compressor.c), and
[biquad.c](https://github.com/velipso/sndfilter/blob/master/src/biquad.c) are the core
algorithms.

I do not understand the biquad math, so please don't ask me any questions :-).  The core formulas
were extracted from
[Biquad.cpp](https://github.com/nwjs/chromium.src/blob/df7f8c8582b9a78c806a7fa1e9d3f3ba51f7a698/third_party/WebKit/Source/platform/audio/Biquad.cpp)
(Chromium source), and cleaned up a bit to make easier to read.

The compressor came from
[DynamicsCompressorKernel.cpp](https://github.com/nwjs/chromium.src/blob/df7f8c8582b9a78c806a7fa1e9d3f3ba51f7a698/third_party/WebKit/Source/platform/audio/DynamicsCompressorKernel.cpp)
(also from Chromium), and cleaned up a bit more.  I swapped out the adaptive release curve and
simplified the knee calculations.  I feel a little more comfortable with that algorithm because
there isn't a whole lot of magical math involved.

The reverb effect is a complete rewrite of [Freeverb3](http://www.nongnu.org/freeverb3/)'s
Progenitor2 algorithm.  It took quite a lot of effort to tear apart the algorithm and rebuild
it, but I'm pretty sure it's right.
