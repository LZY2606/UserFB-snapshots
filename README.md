# TinyMPG

[![Arduino Library](https://img.shields.io/badge/Arduino-Library-blue.svg)](https://www.arduino.cc/reference/en/libraries/)
[![CMake](https://img.shields.io/badge/CMake-Supported-blue.svg)](https://cmake.org/)
[![IDF Component](https://img.shields.io/badge/IDF-Component-blue.svg)](https://github.com/pschatzmann/TinyMPG)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-green.svg)](https://www.gnu.org/licenses/gpl-3.0)

A minimal MPEG-1 (ISO/IEC 11172 part 2) video codec — written from scratch in header-only C++, for microcontrollers such as the ESP32 with  no external library dependencies.

You can also use this library for the compression of images: Compared with raw RGB565 (2 bytes/pixel), a typical MPEG-1 I-frame is roughly 7–20× smaller, depending on image content and encoding settings.

## Performance

Measured, not estimated, QCIF (176x144), **both test clips are all
I-frames** (worst case - see below):

| Board | Decoding fps (`examples/DecodeFromProgmem`) | Encoding fps (`examples/EncodeSyntheticFrame`, `qscale=8`) |
|---|---|---|
| ESP32 (no PSRAM) | 11.1 | 11.5 |
| STM32H750VBT6 | 40.1 | 43.6 |
| x86 (desktop, `-O2`) | ~1900 | ~540 |

Decoding fps is the real per-decoded-picture cost (worst-case within the
benchmark, i.e. `1000000 / max_us`), not the reorder-buffer-diluted
average - see [Memory budget](docs/memory-budget.md#performance-measured-real-hardware)
for the full per-frame breakdown behind each row.

The x86 column is `test/native/bench_native.cpp` (same clip/gradient and
repetition counts as the ESP32 examples, just timed with
`std::chrono`) - build/run it with `cmake --build build --target
bench_native && ./build/test/native/bench_native`. See
[Memory budget](docs/memory-budget.md#performance-measured-real-hardware)
for full details.

## Containers

TinyMPG speaks raw MPEG-1 video elementary streams only (ISO/IEC 11172-2,
delimited by `picture_start_code` sequences) — there's no container
muxing/demuxing in this library, deliberately. To produce a playable MPEG-1 Program Stream from a decoded/encoded elementary stream, or to extract the raw video elementary stream from an existing `.mpg` file, use the [AudioTools](https://github.com/pschatzmann/arduino-audio-tools) project's
`MuxerMPG`/`DemuxerMPG` classes (`ContainerMPG.h`) — see
[Architecture](docs/architecture.md#integration-contract-with-demuxermpg-arduino-audio-tools)
for exactly how the two projects' framing assumptions line up.

## Documentation

- [Architecture](docs/architecture.md) — why this project exists, the
  library survey behind building it from scratch, the full file layout,
  and the integration contract with `DemuxerMPG`.
- [Scope](docs/scope.md) — what's implemented/validated (Phase A: I-frame
  decode, Phase B: P-frame decode, Phase C: B-frame decode, Phase 1:
  intra-only encode) vs. planned (motion-compensated encoding) vs.
  explicitly out of scope.
- [Memory budget](docs/memory-budget.md) — static/heap RAM cost (TBD until
  measured on real hardware, same rule TinyH264 follows: no fabricated
  benchmark numbers).
- [Decoding](docs/decoding.md) — `TinyMPGDecoder` usage.
- [Encoding](docs/encoding.md) — `TinyMPGEncoder` usage.
- [Preparing input with ffmpeg](docs/preparing-input-with-ffmpeg.md) — the
  exact `ffmpeg` command line to produce a stream this decoder can read.
- [Testing](docs/testing.md) — running the native CMake/CTest suite, and
  how the test assets themselves are generated.

## Installation

For Arduino, download this library as a zip and use Library ->
Include Library -> Add .ZIP Library. Or git clone this project into
your Arduino libraries folder, e.g.

```
cd ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/TinyMPG.git
```

No external Arduino library dependencies - the only include beyond the
C++ standard library is `Arduino.h` itself (transitively, via the
Arduino build), and PSRAM support (`PSRAMAllocatorESP32.h`) is opt-in
and self-contained.

For CMake or ESP-IDF projects instead, see [Testing](docs/testing.md)
for `add_subdirectory()`/`EXTRA_COMPONENT_DIRS` usage.
