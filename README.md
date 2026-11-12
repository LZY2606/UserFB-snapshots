# Moog Ladder Filters

[![CMake on multiple platforms](https://github.com/ddiakopoulos/MoogLadders/actions/workflows/cmake-multi-platform.yml/badge.svg?branch=main)](https://github.com/ddiakopoulos/MoogLadders/actions/workflows/cmake-multi-platform.yml)

This project contains different digital implementations of the classic 4-pole, 24 dB/octave analog filter introduced in 1965. The filter is well-regarded to add a nice character to any sound source, either synthesized or acoustic. The ladder structure consists of four one-pole filters and a global negative feedback loop.

The C++11 filter classes do not rely on external libraries and can be used with little to no modification in other DSP projects; the idea of this project is that you choose which variant suits your project best and then copy the parts that you need, rather than included as a monolithic DSP library. Every filter has been modified from its original implementation for code clarity and/or runtime performance. The project includes a test app that will load a sample wav file through each of the implemented filter variants. 

# Filter Tuning & A Word of Warning
Each model is unique. The newest is from 2025 while the oldest dates back over 20 years. Some try to remain true to their analog counterpart, where others are more approximate. The filters have not been rigorously verified for all combinations of cutoff, resonance, and sampling rate. Some are purposely built to self-oscillate, but beware the occasional blow-up with parameters that exceed some undiscovered value. 

# Models & Licenses

“Closed-Source Friendly” indicates if the individual license permits redistribution in a closed-source product (like a VST plugin). Filtered output audio is fair game for any kind of sample library or music production, commercial or otherwise. In the case of copyright-only code, it is possible to contact the original author to request an explicit license.

Implementation | License | Original Source | Closed-Source Friendly
------------- | ------------- | ----------------- | -----------------
Simplified | Custom | DAFX | No
Huovilainen  | LGPLv3 | CSound | If dynamically linked
Stilson | Unlicense | Moog~ by D. Lowenfels | Yes
Microtracker | Unlicense | Magnus Jonsson | Yes
Krajeski | Unlicense | Aaron Krajeski | Yes
MusicDSP | Suggested CC-BY-SA | MusicDSP.org | Yes
Oberheim | Custom | Will Pirkle | Yes
Improved | ISC | Via Author | Yes
RKSimulation | BSD | Bob~ by Miller Puckette | Yes
Hyperion | Unlicense | Via Author | Yes

# Analysis

A variety of filter tests and analysis are implemented as a Python script. After building the RunFilters target, use like so:
```
python scripts/filter_verification.py --runfilters build/Release/RunFilters.exe --tests all --filters all --os 0,4 --verbose
```

An interactive html dashboard can be generated from the output of the `filter_verification.py` script and run like this (example):
```
python scripts/dashboard_generator.py filter_validation/2026-01-24_194311
```

The dashboard generator is imported as a module into the analysis, so you can also run both in one go: 
```
python scripts/filter_verification.py -r build/Release/RunFilters.exe --tests all --filters all --dashboard
```


# Voice-Parallel Bank: RKSimulationBank

`RKSimulationBank` runs many independent `RKSimulationMoog` voices at once. Instead of storing one filter object per voice and calling `Process` on each, the bank keeps every voice's four integration states in one stage-major layout and applies the same Runge–Kutta arithmetic to several voices per instruction. On AArch64 the `Auto` backend uses real NEON across voices (two voices per double-precision vector, with a scalar tail for odd counts); elsewhere `Auto` selects a portable scalar backend. The per-voice math, including the `double` integration states and the float round-trip in the saturation `clip`, matches `RKSimulationMoog`.

Buffers are *planar*: each voice points at its own independent, in-place mono float buffer. No 16-byte alignment is required, and the voice count does not need to be a multiple of the vector width.

```cpp
#include "RKSimulationBank.h"
#include <vector>

constexpr std::size_t voices = 12;
constexpr uint32_t    frames = 256;

RKSimulationBank bank(48000.0f, voices, RKSimulationBank::Backend::Auto);

// Per-voice parameters (initial cutoff/resonance already match RKSimulationMoog).
for (std::size_t v = 0; v < voices; ++v)
{
    bank.SetCutoff(v, 200.0f + 80.0f * v);
    bank.SetResonance(v, 1.5f);
}

// One independent buffer per voice (planar layout).
std::vector<std::vector<float>> buffers(voices, std::vector<float>(frames));
std::vector<float *> planar(voices);
for (std::size_t v = 0; v < voices; ++v)
    planar[v] = buffers[v].data();

// Fill inputs, then process all voices for the whole block, in place.
for (std::size_t v = 0; v < voices; ++v)
    for (uint32_t f = 0; f < frames; ++f)
        buffers[v][f] = /* voice v sample f */ 0.0f;

bank.Process(planar.data(), frames);

// Reset one voice's four integration states without touching its parameters
// or the other voices' ringing tails.
bank.ResetVoice(3);
```

Parameter updates take effect from the first frame of the next `Process` call, and `Process(nullptr, 0)` is a safe no-op. Selecting `Backend::Neon` on a target without NEON (or when `MOOG_BANK_FORCE_SCALAR` is defined) throws `std::invalid_argument`. Regression coverage for numeric parity, voice independence and the native NEON kernel lives in `tests/target_regression.cpp` and is driven by `bash checks/target.sh`.

# ToDo

Community contributions are welcome.

* Several filters have extra parameters that could be exposed (drive, thermal tuning coefficients, etc).
* Many filters may also be easily modified for HPF or other types of output.

# License
If not otherwise stated in the header of a file, all other code in this project is released under the Unlicense.
