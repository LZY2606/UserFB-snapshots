# PandaResampler

## Description

PandaResampler is a fast C++ (SSEified) factor 2 resampler for audio signals.
It is easy to integrate into projects since it supports header only mode.

PandaResampler is currently *experimental* which means that right now there
still can be API changes that break backward compatibility. API documentation
is available here:  https://space.twc.de/~stefan/pandaresampler/api-0.2.1

## Using the Shared Library

To use PandaResampler as shared library, build it using

    meson setup build                      # configure the build
    meson compile -C build                 # compile PandaResampler
    sudo meson install -C build            # install PandaResampler
    # you may need to run 'sudo ldconfig' before you can use the library

Then, you can use `pkg-config` to get the necessary compiler flags to use
the shared library:

 * `pkg-config --cflags pandaresampler` to get the flags for compiling
 * `pkg-config --libs pandaresampler` for linking

## Header only C++ Library

If you do not want to link against any library, you can use

 * `pkg-config --cflags pandaresampler` and the define `-DPANDA_RESAMPLER_HEADER_ONLY`

while compiling your code, this will not require any library to be present to
compile or run your compiled program.

If you prefer, you can simply copy everything from the `include/` directory of
the pandaresampler to your project to make it fully self-contained and avoid
using `pkg-config`.

## License

PandaResampler is released under
[MPL-2.0](https://github.com/swesterfeld/pandaresampler/blob/master/MPL-2.0.txt),
however it also contains code from hiir which is licensed under
[WTFPL2](https://github.com/swesterfeld/pandaresampler/blob/master/include/pandaresampler/hiir/license.txt).

## Periodic steady-state resampling (PeriodicPlan)

When a waveform is a short loop (for example a wavetable oscillator) that is
resampled with the IIR halfband filters, an ordinary `Resampler2` starts with
zero filter memory. The IIR recursion then needs many repetitions of the loop
before its start-up transient has decayed, so the first rendered period does
not match the later ones.

`PandaResampler::PeriodicPlan` removes that ramp-up. It is a multi-channel,
scalar (FPU) IIR plan for ratios 2/4/8 and precisions `PREC_48DB`,
`PREC_96DB` and `PREC_144DB`. At construction it takes a planar PCM source
loop (the data is copied) and computes the *exact periodic steady state* of
every all-pass stage with a finite periodic boundary calculation. It does not
warm up for a fixed number of turns, iterate to an error threshold, cross-fade
the seam, or play back a precomputed table. `render()` always runs the real
IIR recursion with the production coefficients and state topology; it never
allocates heap memory and its working set is independent of the block length.

### Output period and odd loop lengths

`period()` returns the length, in output frames, of one fully closed period.

* Upsampling by R: `P = N * R`.
* Downsampling by R: `P = N / gcd(N, R)`.

For downsampling, the decimation lattice and the filter histories only close
together after `R / gcd(N, R)` repetitions of the source loop. The source
pointer returning to frame 0 is therefore not, in general, a reset point: with
an odd `N` (for example N=3, R=4) the plan internally walks several source
loops before every stage memory and the decimation phase simultaneously
return to their starting values. All of this is handled inside the plan; the
caller only sees the repeating output period `P`.

### Causal delay is preserved

The steady-state calculation only chooses the initial values of the filter
memories. It does not alter the filter coefficients or reposition the output
phase, so the ordinary causal group delay of the IIR resampler is kept and no
extra group-delay compensation is applied. The conversion phase is identical
to running `Resampler2::process_block()` beginning at source frame 0.

### PeriodicPlan::reset() versus Resampler2::reset()

These are different operations:

* `Resampler2::reset()` clears the filter memories to zero. That reproduces
  the original cold start, including the full zero-state ramp-up transient.
* `PeriodicPlan::reset()` (and `seek(0)`) restores the already solved periodic
  steady-state memories, so the next output sample is again the first sample
  of the closed, infinitely repeated response. `seek(frame)` first reduces the
  cursor modulo the period with integer arithmetic (positions above 2^53 are
  never rounded through `double`) and then advances within a single period, so
  its cost does not grow with the absolute frame number.

A device-free usage example is provided in
[`examples/periodic_example.cc`](examples/periodic_example.cc).
