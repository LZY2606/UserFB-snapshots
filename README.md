<img src="https://www.wavpack.com/logos/wavpacklogo-with-theme.svg" width="250"></img>

Hybrid Lossless Wavefile Compressor

Copyright (c) 1998 - 2025 David Bryant.

All Rights Reserved.

Distributed under the [BSD Software License](https://github.com/dbry/WavPack/blob/master/license.txt).

---

This [repository](https://github.com/dbry/WavPack) contains all of the source code required to build the WavPack library (_libwavpack_), the associated command-line programs, and a few example plugins.

Additional references:

* [Official website](http://wavpack.com/)
* [Binaries](http://wavpack.com/downloads.html#binaries)
* [Other sources](http://wavpack.com/downloads.html#sources)
* [Documentation](http://wavpack.com/downloads.html#documentation)
* [Test suite](http://www.rarewares.org/wavpack/test_suite.zip)
* [Logos](http://wavpack.com/downloads.html#logos)

---

## Fuzzing & Build Status

| Branch         | Status                                                                                                                                                                          |
|----------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `master`       | [![Fuzzing Status](https://oss-fuzz-build-logs.storage.googleapis.com/badges/wavpack.svg)](https://bugs.chromium.org/p/oss-fuzz/issues/list?sort=-opened&can=1&q=proj:wavpack)  |
| `master`       | [![Build Status](https://github.com/dbry/WavPack/actions/workflows/build.yml/badge.svg?event=push)](https://github.com/dbry/WavPack/actions)                                                                     |

---

## Building

### Windows

There are solution and project files for Visual Studio 2019, and additional source code to build the [CoolEdit/Audition](https://github.com/dbry/WavPack/tree/master/audition) plugin and the [Winamp](https://github.com/dbry/WavPack/tree/master/winamp) plugin.

The CoolEdit/Audition plugin provides a good example for using the library to both read and write WavPack files, and the Winamp plugin makes extensive use of APEv2 tag reading and writing.

Both 32-bit and 64-bit platforms are provided.

### Linux

To build everything on Linux, type:

1. `./configure`
2. `make`
3. Optionally, `make install`, to install into `/usr/local/bin`

If you are using the code directly from Git (rather than a distribution) then you will need to do a `./autogen.sh` instead of the configure step. If assembly optimizations are available for your processor they will be automatically enabled, but if there is a problem with them then use the `--disable-asm` option to revert to pure C.

For Clang-based build systems (Darwin, FreeBSD, etc.), Clang version 3.5 or higher is required.

If you get a WARNING about unexpected _libwavpack_ version when you run the command-line programs, you might try using `--enable-rpath` to hardcode the library location in the executables, or simply force static linking with `--disable-shared`.

There is now a CLI program to do a full suite of stress tests for _libwavpack_, and this is particularly useful for packagers to make sure that the C code and assembly language optimizations are working correctly on various platforms. It is built and a quick test is run with `make check`. Once the program is built then more extensive testing can be done with `wvtest --default` and there is also a seeking test. On Windows a third-party Pthreads library is required, so I am not including this in the build for now.

---

## Assembly

Assembly language optimizations are provided for x86 and x86-64 (AMD64) processors (encoding and decoding) and ARMv7 (decoding only).

The x86 assembly code includes a runtime check for MMX capability, so it will work on legacy i386 processors.

## Documentation

There are four documentation files contained in the distribution:

| File                         | Description                                                                                                                                                   |
|------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------|
| [doc/wavpack_doc.html](https://github.com/dbry/WavPack/blob/master/doc/wavpack_doc.html)         | Contains user-targeted documentation for the command-line programs.                                                                                            |
| [doc/WavPack5PortingGuide.pdf](https://github.com/dbry/WavPack/blob/master/doc/WavPack5PortingGuide.pdf) | This document is targeted at developers who are migrating to WavPack 5, and it provides a short description of the major improvements and how to utilize them. |
| [doc/WavPack5LibraryDoc.pdf](https://github.com/dbry/WavPack/blob/master/doc/WavPack5LibraryDoc.pdf)   | Contains a detailed description of the API provided by WavPack library appropriate for reading and writing WavPack files and manipulating APEv2 tags.              |
| [doc/WavPack5FileFormat.pdf](https://github.com/dbry/WavPack/blob/master/doc/WavPack5FileFormat.pdf)   | Contains a description of the WavPack file format, including details needed for parsing WavPack, blocks, and interpreting the block header and flags.            |

There is also a description of the WavPack algorithms in the forth edition of David Salomon's book "Data Compression: The Complete Reference". This section can be found here: www.wavpack.com/WavPack.pdf

## Per-channel hybrid bitrate plans

A hybrid encoder normally applies one bitrate to every channel. When the
channels carry different material (for example a main microphone plus an
auxiliary track), a *bitrate plan* lets you assign an independent budget to
each channel and change those budgets at absolute sample positions, all inside
a single WavPack hybrid stream. You keep feeding PCM with the normal
`WavpackPackSamples()` calls — there is no need to split the input into several
files or to call `WavpackFlushSamples()` at the change points; the library
ends every audio block exactly on a plan cut.

A plan is built from `WavpackHybridBitratePoint` entries. Each entry gives a
sample index and one Q8 (256ths of a bit/sample) budget per channel, in
interleaved channel order. The first entry must be at index 0, the indices are
strictly increasing, and each budget is between 576 (2.25 bits/sample) and
6144 (24.0 bits/sample). The plan is installed after
`WavpackSetConfiguration64()` and before `WavpackPackInit()`; on acceptance the
whole plan is copied, so the caller's storage can be reused immediately.

```c
#include "wavpack.h"

/* two channels, three budget changes at frames 0, 1000 and 5000 */
static const uint16_t budget_0 [2] = { 1024, 2048 };  /* 4.0 and 8.0 bits/sample */
static const uint16_t budget_1 [2] = { 2048, 1024 };
static const uint16_t budget_2 [2] = { 3072, 3072 };  /* 12.0 bits/sample each   */

static const WavpackHybridBitratePoint plan [3] = {
    {    0, budget_0 },
    { 1000, budget_1 },
    { 5000, budget_2 }
};

int setup_plan_encoder (WavpackContext *wpc, WavpackConfig *config, int64_t total_frames)
{
    if (!WavpackSetConfiguration64 (wpc, config, total_frames, NULL))
        return 0;                                   /* inspect WavpackGetErrorMessage() */

    /* accepted only for hybrid 1-8 channel 16/24-bit integer PCM; a rejected
       call leaves any previously accepted plan (and the encoder state) intact */
    if (!WavpackSetHybridBitratePlan (wpc, plan, sizeof (plan) / sizeof (plan [0])))
        return 0;

    if (!WavpackPackInit (wpc))
        return 0;

    /* feed PCM in arbitrary batches from here; block boundaries are aligned to
       the plan cuts automatically, and WavpackFlushSamples() needs no help */
    return 1;
}
```

Passing a count of 0 (with a NULL point array) clears any previously installed
plan. Streams written with a plan still use the standard WavPack format: an
existing decoder reconstructs the original PCM sample-for-sample when the
`.wvc` correction file is present and decodes normally as lossy audio from the
`.wv` file alone.

## Portability

This code is designed to be easy to port to other platforms.

It is endian-agnostic and usually uses callbacks for I/O, although there's a convenience function for reading files that accepts filename strings and automatically handles correction files.

On Windows, there is now an option to select UTF-8 instead of ANSI.

To maintain compatibility on various platforms, the following conventions are used:
* `char` must be 8-bits (`signed` or `unsigned`).
* `short` must be 16-bits.
* `int` and `long` must be at least 32-bits.

## Design

The code's modules are organized in such a way that if major chunks of the functionality are not referenced (for example, creating WavPack files) then link-time dependency resolution should provide optimum binary sizes.

However, some functionality could not be easily excluded in this way and so there are additional macros that may be used to further reduce the size of the binary. Note that these must be defined for all modules:

| Macros          | Description                                                                                                |
|-----------------|------------------------------------------------------------------------------------------------------------|
| `NO_SEEKING`    | To not allow seeking to a specific sample index (for applications that always read entire files).          |
| `NO_TAGS`       | To not read specified fields from ID3v1 and APEv2 tags, and not create or edit APEv2 tags.                 |
| `ENABLE_LEGACY` | Include support for Wavpack files from before version 4.0. This was eliminated by default with WavPack 5. |
| `ENABLE_DSD`    | Include support for DSD audio. New for WavPack 5 and the default, but obviously not universally required. |

Note that this has been tested on many platforms.

## Tiny Versions

There are alternate versions of this library available specifically designed for resource limited CPUs (i.e., portable devices).

There is the _Tiny Decoder_ library which works with less than 32k of code and less than 4k of data, and has assembly language optimizations for the ARM and Freescale ColdFire CPUs. It is also the basis for a WebAssembly WavPack player [on GitHub](https://github.com/soiaf/WebAssembly-WavPack). The Tiny Decoder can be downloaded [here](https://www.wavpack.com/tiny_decoder.zip).

The _Tiny Encoder_ is also designed for embedded use and handles the pure lossless, lossy, and hybrid lossless modes. It can be downloaded [here](https://www.wavpack.com/tiny_encoder.zip).

Neither of these versions use any memory allocation functions, nor do they require floating-point arithmetic support. Version of them appear in the [Rockbox project](https://www.rockbox.org/).

---

Questions or comments should be directed to david@wavpack.com.

You may also find David on GitHub as [dbry](https://github.com/dbry).
