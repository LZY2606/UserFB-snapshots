## A Collection of Useful C++ Classes for Digital Signal Processing

> "Techniques for digital signal processing are well guarded and held
> close to the chest, as they have valuable applications for multimedia
> content. The black art of Infinite Impulse Response ("IIR") filtering
> has remained veiled in secrecy with little publicly available source
> code...until now."

<a href="https://github.com/downloads/vinniefalco/DSPFilters/DSPFiltersComplete.zip">
<img src="https://github.com/vinniefalco/vinniefalco.github.com/raw/master/images/DownloadNow.png">
</a>

### What is this?

Building on the work of cherished luminaries such as Sophocles Orfanidis,
Andreas Antoniou, Martin Holters, and Udo Zolzer, this library harnesses
the power of C++ templates to solve a useful problem in Digital Signal
Processing: the realization of multichannel IIR filters of arbitrary order
and prescribed specifications with various properties such as Butterworth,
Chebyshev, Elliptic, and Optimum-L (Legendre) responses. The library is
provided under the MIT license and is therefore fully compatible with
proprietary usage.

Classes are designed as independent re-usable building blocks. Use some or
all of the provided features, or extend the functionality by writing your
own objects that plug into the robust framework. Only the code that you
need will get linked into your application. Here's a list of features:

- Exclusive focus on IIR filters instead of boring FIR filters
- Complete implementation of all "RBJ Biquad" Cookbook filter formulas
- Butterworth, Chebyshev, Elliptic, Bessel, Legendre designs
- Low Pass, High Pass, Band Pass, Band Stop transformations
- Low, High, and Band Shelf filter implementations for most types
- Smooth interpolation of filter settings, pole/zeros, and biquad
  coefficients to achieve seamless parameter changes
- Representation of digital filters using poles and zeros
- Realization using Direct Form I, Direct Form II, or user provided class
- Fully factored to minimize template instantiations
- "Design" layer provides runtime introspection into a filter
- Utility template functions for manipulating buffers of sample data
- No calls to malloc or new, great for embedded systems
- No external dependencies, just the standard C++ library!

Using these filters is easy:

    // Create a Chebyshev type I Band Stop filter of order 3
    // with state for processing 2 channels of audio.
    Dsp::SimpleFilter <Dsp::ChebyshevI::BandStop <3>, 2> f;
    f.setup (3,    // order
             44100,// sample rate
             4000, // center frequency
             880,  // band width
             1);   // ripple dB
    f.process (numSamples, arrayOfChannels);

An accompanying demonstration program that works on most popular
platforms by using the separately licensed Juce application framework
(included), exercises all the functionality of the library, including
these features:

- Dynamic interface creates itself using filter introspection capabilities
- Audio playback with real time application of a selected filter
- Live time stretching and amplitude modulation without clicks or popping
- Charts to show magnitude, phase response and pole/zero placement
- Thread safety "best practices" for audio applications 

This is the provided DSP Filters Demo application, which demonstrates the
features of the library and uses the Juce application framework to run on
all popular platforms:

<img src="https://github.com/vinniefalco/DSPFilters/raw/gh-pages/images/DSPFiltersScreenshot.png">

If you've been searching in futility on the Internet for some source code
for implementing high order filters, then look no further because this is
it! Whether you are a student of C++ or digital signal processing, a writer
of audio plugins, or even a VST synthesizer coder, "A Collection of Useful
C++ Classes for Digital Signal Processing" might have something for you!

### DSP Filters on the Web

DSP Filters Official Discussion Forum<br>
http://www.kvraudio.com/forum/viewtopic.php?t=249926

DSP and Audio Plugin Discussion Forum<br>
http://www.kvraudio.com/forum/viewforum.php?f=33

Juce Official Discussion Forum<br>
http://www.rawmaterialsoftware.com/index.php

Juce Official Site<br>
http://www.rawmaterialsoftware.com

### Terms and Conditions
DSP Filters Library and DSP Demo Application Copyright (c) 2009 by
[Vinnie Falco](http://github.com/vinniefalco)<br>
Source code is provided under the
[MIT License](http://www.opensource.org/licenses/mit-license.php)<br>
The [Juce Library](http://www.rawmaterialsoftware.com) is licensed
separately, from [Raw Material Software](http://rawmaterialsoftware.com).

### History matched modulation

When filter parameters are automated (for example a cutoff sweep), the
smoothed filter updates its coefficients once per sample. With constant
parameters the Direct Form I, Direct Form II and Transposed Direct Form II
realizations sound identical, but during a sweep they can differ: each form
keeps its internal delay variables in a different representation, and simply
carrying the previous coefficient system's delays into the new system is not
equivalent across forms.

`SmoothedFilterDesign` offers an optional **history matched modulation** mode
that makes parameter automation mean the same thing regardless of the chosen
state realization. The mode is off by default and the legacy behavior and
output are unchanged.

```cpp
Dsp::SmoothedFilterDesign<Dsp::RBJ::Design::LowPass, 2, Dsp::DirectFormII> f (128);
f.setParams (params);                         // first set takes effect immediately
f.enableHistoryMatchedModulation (true);     // opt in before processing audio
f.process (numSamples, arrayOfChannels);
```

How it works:

- Coefficients are still advanced one sample at a time through the existing
  transition parameter machinery. For a transition of N samples the first
  processed sample sits at 1/N of the ramp and the last at exactly the target.
- Every second order section applies the coefficients in force at that sample
  to the section's *real* last two inputs and last two outputs — its direct
  difference equation. The previous coefficient system's internal delays are
  never reused as the new system's state; the selected realization's delays are
  re-seeded from the canonical section history before each output. The chosen
  `StateType` still computes every sample through its own `process1()`.
- Each channel and each cascade section keeps independent history, and a
  later section receives the previous section's actual output within the same
  sample. History keeps accumulating while the coefficients are constant, when
  a sweep finishes and processing returns to ordinary block processing, and
  across later sweeps.
- Parameters are placed from an absolute ramp fraction, so the trajectory is
  identical for any block size (whole buffer versus 1/7/113-sample chunks).
  Zero-length `process()` calls do not advance anything, and retargeting a
  sweep mid-way continues from the position actually reached.

Interface contract:

- `enableHistoryMatchedModulation(bool enabled)` may only be called before any
  non-empty input has been processed, or immediately after `reset()`. Calling
  it at any other time throws `std::logic_error`. Enabling the mode for a state
  type without a history-matched transform also throws `std::logic_error`.
- `reset()` clears the audio history but keeps the selected mode. With the
  mode enabled it also cancels any unfinished transition and settles at the
  parameters currently in use. With the mode disabled the existing reset
  behavior is preserved.
- No heap allocation occurs in `setParams()` or `process()`; per-section
  history is fixed size, and per-sample work grows only with channel and
  section count.

Supported in this mode: `RBJ::Design::LowPass`, `RBJ::Design::HighPass`, and
the fixed fourth-order `Butterworth::Design::LowPass<4>`, using
`DirectFormI`, `DirectFormII`, or `TransposedDirectFormII`, with one or two
channels and `float` or `double`. The conformance test
(`checks/history_modulation.cpp`, run by `checks/history_modulation.sh`)
compares all design/state combinations against an independent double
precision reference; see `checks/history_modulation_sweep_example.cpp` for a
parameter automation example.

