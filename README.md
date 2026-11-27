# Signalsmith Stretch: C++ pitch/time library

This is a C++11 library for pitch and time stretching, using the final approach from the ADC22 presentation [Four Ways To Write A Pitch-Shifter](https://www.youtube.com/watch?v=fJUmmcGKZMI).

It can handle a wide-range of pitch-shifts (multiple octaves) but time-stretching sounds best for more modest changes (between 0.75x and 1.5x).  There are some audio examples and an interactive web demo on the [main project page](https://signalsmith-audio.co.uk/code/stretch/).

## How to use it

Just include `signalsmith-stretch.h` where needed:

```cpp
#include "signalsmith-stretch.h"

signalsmith::stretch::SignalsmithStretch<float> stretch;
```

### Configuring

The easiest way to configure is a `.preset???()` method:

```cpp
stretch.presetDefault(channels, sampleRate);
stretch.presetCheaper(channels, sampleRate);
```

If you want to try out different block sizes for yourself. you can use `.configure()` manually:

```cpp
stretch.configure(channels, blockSamples, intervalSamples);

// query the current configuration
int block = stretch.blockSamples();
int interval = stretch.intervalSamples();
```

### Processing and resetting

To process a block, call `.process()`:

```cpp
float **inputBuffers, **outputBuffers;
int inputSamples, outputSamples;
stretch.process(inputBuffers, inputSamples, outputBuffers, outputSamples);
```

The input/output buffers cannot be the same, but they can be any type where `buffer[channel][index]` gives you a sample.  This might be `float **` or a `double **` or some custom object (e.g. providing access to an interleaved buffer), regardless of what sample-type the stretcher is using internally.

To clear the internal buffers:

```cpp
stretch.reset();
```

### Pitch-shifting

```cpp
stretch.setTransposeFactor(2); // up one octave

stretch.setTransposeSemitones(12); // also one octave
```

You can set a "tonality limit", which uses a non-linear frequency map to preserve a bit more of the timbre:

```cpp
stretch.setTransposeSemitones(4, 8000/sampleRate);
```

Alternatively, you can set a custom frequency map, mapping input frequencies to output frequencies (both normalised against the sample-rate): 

```cpp
stretch.setFreqMap([](float inputFreq) {
	return inputFreq*2; // up one octave
});
```

### Time-stretching

To get a time-stretch, hand differently-sized input/output buffers to .process(). There's no maximum block size for either input or output.

Since the buffer lengths (inputSamples and outputSamples above) are integers, it's up to you to make sure that the block lengths average out to the ratio you want over time.

### Latency

Latency is particularly ambiguous for a time-stretching effect. We report the latency in two halves:

```cpp
int inputLatency = stretch.inputLatency();
int outputLatency = stretch.outputLatency();
```

You should be supplying input samples slightly ahead of the processing time (which is where changes to pitch-shift or stretch rate will be centred), and you'll receive output samples slightly behind that processing time.

### Split computation

All of the `.preset???()` and `.configure()` methods have an optional `splitComputation` flag.  When enabled, this introduces one extra interval of output latency, and uses this to spread the computation out more evenly.

Without this (as is common for spectral processing) the library will occasionally do a bunch of computation all at once, to compute the next spectral block of audio.  This is often fine, when audio's being processed across multiple threads with a decent amount of buffering (like mixing in a DAW), but if you're in a stricter situation then this flag might help. 

#### Automation

To follow pitch/time automation accurately, you should give it automation values from the current processing time (`.outputLatency()` samples ahead of the output), and feed it input from `.inputLatency()` samples ahead of the current processing time.

### Seeking and starting

You can use `.seek()` which lets you move around the input audio, by providing a bunch of input samples.  You should ideally provide at least (one block-length + one interval) of input data:

```cpp
stretch.seek(inputBuffers, inputSamples, playbackRateHint);
```

At the very start of playback (or after a `.reset()`), the current processing time is `.inputLatency()` samples *before* the first input samples you give it.  You therefore might want to call `.seek()` to provide the first `inputSamples = stretch.inputLatency()` samples of input, so that the processing time matches the start of the input (meaning your pre-roll output is only `.outputLatency()` samples long).

### Ending

If you're processing a fixed-length sound (instead of an infinite stream), you'll reach the end of your input, but still have some pending output.  You should first make sure the processing time gets to the end, by passing an additional `.inputLatency()` samples of silence to `.process()` (similar to using `.seek()` at the beginning).

You can then read the final part of the output using `.flush()`.  It's recommended to read at least `.outputLatency()` samples of output:

```cpp
stretch.flush(outputBuffers, outputSamples);
``` 

Using `.seek()`/`.flush()` like this, you can perform an exact time-stretch on a fixed-length sound, and your result will have `.outputLatency()` of pre-roll.

### Formant compensation

```cpp
stretch.setFormantFactor(1.2); // up ~3 semitones

stretch.setTransposeSemitones(3);
```

Both of those methods both have an optional `compensatePitch` flag.  Enabling this adjust for the pitch-shift (or non-linear map) when correcting/shifting formants.

The formant correction is not a sharp as monophonic algorithms (such such as PSOLA).  It also needs you to give it a rough estimate of fundamental frequency (relative to Nyquist):

```cpp
// if 200Hz is the middle-register of the instrument
stretch.setFormantBase(200/sampleRate);
```

## Multi-voice harmonizer

When the same input becomes several harmonised voices (each sent to its own pan/mix bus), running several independent `SignalsmithStretch` instances repeats the same forward analysis once per voice.  `SignalsmithHarmonizer` shares that analysis:

```cpp
#include "signalsmith-harmonizer.h"

signalsmith::stretch::SignalsmithHarmonizer<float> harmonizer(seed);
// channels, voices, window, hop, split-computation flag
harmonizer.configure(2, 4, 512, 128, /*splitComputation=*/false);

// Every voice starts at transpose 1, formant 1, compensation off.
harmonizer.setVoice(0, 1.0,  1.0,  false);
harmonizer.setVoice(1, 1.25, 1.05, true);
// ...
harmonizer.setFormantBase(120.0f/sampleRate); // shared formant base
```

`process()` takes planar input with `channels` planes and planar output with `voices*channels` planes: voice `v`, channel `c` lands in `outputs[v*channels + c]`.  These are the per-voice signals *before* any mixing — no limiting or normalisation is applied, so you keep full control of gain and pan downstream.

The shared work is the forward STFT only: for a given input position the current spectrum (and, when needed, the previous-time re-analysis) is computed once per channel, regardless of voice count.  Each voice keeps its own private frequency map, peak targets, phase predictions, formant-corrected energies and overlap-add history, so a voice's energy correction can never leak into another, and voices are not treated as extra channels of a single signal.  With static settings each voice is numerically identical to a standalone `SignalsmithStretch` configured with the same seed/window/mode.

### Latency

`inputLatency()` / `outputLatency()` match a standalone stretcher with the same configuration.  In split-computation mode the output latency still includes the extra one interval of delay, and the per-hop work (including every voice) is spread across the output interval rather than computed all at once and delayed.

### Automation latching

`setVoice()` and `setFormantBase()` are called between `process()` calls.  All pending settings are latched together once, at the start of each new synthesis hop:

* the hop currently being computed (including a half-finished hop in split mode) always completes with the old, latched settings;
* repeated calls before the next hop collapse to the last value, so a hop never mixes an old pitch map with a new phase prediction;
* changing one voice never resets the others, nor clears that voice's own phase/tail history.

### Seeking, flush and reset

`seek()`, `flush()` and `reset()` follow the same semantics and finite-sound trimming rules as `SignalsmithStretch`.  The shared input advances exactly once on a `seek()`, and `reset()` clears all history (shared analysis and every voice's private state) while keeping the configuration and current voice settings.  After `configure()`, all workspaces are fixed-size: `process()`/`seek()`/`flush()`/`reset()` perform no heap allocation.

### Example

`examples/harmonizer-example.cpp` generates a short voice-like tone, runs three voices through one shared analysis, and mixes them with per-voice gains and equal-power pan into a stereo 16-bit WAV:

```sh
c++ -std=c++11 -O2 -I. -Ivendor/linear/include examples/harmonizer-example.cpp -o harmonizer-example
./harmonizer-example harmonizer.wav
```

## Compiling

⚠️ This has mostly been tested with AppleClang (Mac), and MSVC (Windows).  We aim to support other compilers though, so please get in touch if you have any problems.

Enabling `-ffast-math` (or equivalent) is fine, unless you're using Apple Clang 16.0.0 (in which case it will deliberately break, because that version generates incorrect SIMD code).

The algorithm has a lot of number-crunching, so Debug builds are much slower (up to 10x).  If possible, you might want to enable optimisation where Stretch is used, even in Debug builds.

### Dependencies and `#define`s

This uses the [Signalsmith Linear](https://github.com/Signalsmith-Audio/linear) library for FFTs and other speedups.  There are [flags]([Linear repo](https://github.com/Signalsmith-Audio/linear?tab=readme-ov-file#building)) to use faster FFT implementations (`SIGNALSMITH_USE_ACCELERATE`/`SIGNALSMITH_USE_IPP`/`SIGNALSMITH_USE_PFFFT`/`SIGNALSMITH_USE_PFFFT_DOUBLE`), and corresponding CMake options.

## License

Released under the [MIT License](LICENSE.txt) - get in touch if you need anything else.

## Other environments / languages

There's a Web Audio release in [`web/`](web/) (WASM/AudioWorklet), also available on [NPM](https://npmjs.com/package/signalsmith-stretch).  This has its own (higher-level) API, but the stretching algorithm will remain in-sync with the C++ library.

There's a [Python binding](https://pypi.org/project/python-stretch/) written/published by [Gregorio Andrea Giudici](https://github.com/gregogiudici/python-stretch).  This is used as the default pitch/time method in [Audiomentations](https://iver56.github.io/audiomentations/).

There's a [Rust wrapper](https://crates.io/crates/signalsmith-stretch) by [Colin Marc](https://github.com/colinmarc/signalsmith-stretch-rs).

## Thanks

We'd like to particularly thank the following people who sponsored specific features or improvements:

* **Future Audio Workshop**: chunked-computation mode
* **Jochem de Jong**: formant shifting/compensation
* **Metronaut**: web audio (JS/WASM) release
* **Daniel L Bowling** and the Stanford School of Medicine: web audio improvements

We're also grateful for the following community contributions:

* **Steve MacKinnon** for finding/resolving a bug in `.reset()`
