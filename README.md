# h264bsd [![Build Status](https://github.com/oneam/h264bsd/actions/workflows/build.yml/badge.svg)](https://github.com/oneam/h264bsd/actions/workflows/build.yml)

This is a software-based library that was extracted from the Android project with the intention of being used elsewhere.

Some modifications have been made to the original project in order to remove the top-level API, add an alloc and free for encoder storage, convert to ARGB format, and add optimizations for certain platforms.

The intention is to provide a simple H.264 decoder that can be easily invoked from [ffi](http://en.wikipedia.org/wiki/Foreign_function_interface) systems.

## Implementation Notes

Currently, the process of decoding modifies the input data. This has tripped me a few times in the past so others should be aware of it.

The decoder only works nicely if it has a single consistent stream to deal with. If you want to change the width/height or restart the stream with a new access unit delimiter, it's better to shutdown and init a new decoder.

## High profile frame-inter 8×8 transform

This tree adds a narrow, native C High-profile path for progressive,
frame-coded H.264 material that uses 8×8 transforms only on **P-frame inter
macroblocks**. The supported High configuration is:

* `profile_idc = 100`, 8-bit YUV 4:2:0;
* `frame_mbs_only_flag = 1`;
* CAVLC (`entropy_coding_mode_flag = 0`);
* one slice group;
* POC types 0 and 2;
* short-term reference I/P pictures;
* no SPS/PPS custom scaling lists (the flat 16 matrix is used);
* `qpprime_y_zero_transform_bypass_flag = 0`;
* equal first and second chroma QP offsets.

The decoder parses PPS `transform_8x8_mode_flag` and each eligible macroblock's
`transform_size_8x8_flag`. Eligible P macroblock types are `P_L0_16x16`,
`P_L0_L0_16x8`, `P_L0_L0_8x16`, and `P_8x8`/`P_8x8ref0` when all four
sub-partitions are 8×8. Smaller sub-partitions, luma CBP zero, and P-Skip use
the old 4×4 syntax/path and do not consume an 8×8 flag. An `I_NxN` flag of zero
continues through the original I4×4 path; `transform_size_8x8_flag = 1`
(Intra8×8) is explicitly rejected.

For an 8×8 luma block, the four CAVLC groups of 16 coefficients retain their
own `TotalCoeff` values for later `nC` derivation, then are de-interleaved into
one natural-coordinate 8×8 coefficient matrix. The implementation performs
position-dependent inverse scaling for the flat default matrix, the specified
integer 8×8 inverse transform and rounding, and then adds the result to the
existing motion prediction. Chroma still uses the original 4×4/DC chain. The
8×8 flag is also stored in macroblock reference/deblocking state: quarter-MB
lines inside an 8×8 transform are not treated as residual transform edges,
while 8-pixel, macroblock-outer, and chroma edges retain their normal handling.

Unsupported High tools return a decoder error rather than being concealed as a
successful picture. These include Intra8×8, CABAC, custom scaling matrices,
transform-bypass/lossless coding, 10/12-bit or 4:2:2/4:4:4 formats, multiple
slice groups in the new High path, B pictures, and other High profiles outside
the stated `profile_idc = 100` subset. The original Baseline API and decoding
behavior are retained; `bash task_support/run.sh baseline` checks the fixed
production I/P comparison.

### C call example

Feed a complete mutable Annex-B access unit (SPS/PPS and slice NALs are all
accepted in the same buffer) through the public API:

```c
#include "h264bsd_decoder.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

int decode_annexb(storage_t *dec, uint8_t *stream, size_t len, uint32_t id)
{
    size_t off = 0;

    while (off < len) {
        u32 used = 0;
        u32 rc = h264bsdDecode(dec, stream + off, (u32)(len - off),
                               id, &used);
        if (rc >= H264BSD_ERROR)
            return -1;

        /* A parameter-set activation can ask the caller to repeat the
         * already-extracted slice NAL once; the retry reports its bytes. */
        if (used == 0) {
            rc = h264bsdDecode(dec, stream + off, (u32)(len - off),
                               id, &used);
            if (rc >= H264BSD_ERROR || used == 0)
                return -1;
        }
        off += used;

        for (;;) {
            u32 pic_id = 0, is_idr = 0, errors = 0;
            u8 *yuv = h264bsdNextOutputPicture(dec, &pic_id, &is_idr,
                                               &errors);
            int w, h;
            if (!yuv)
                break;
            w = 16 * (int)h264bsdPicWidth(dec);
            h = 16 * (int)h264bsdPicHeight(dec);
            /* Planar I420: w*h Y, w*h/4 Cb, w*h/4 Cr. */
            fwrite(yuv, 1, (size_t)w * h * 3 / 2, stdout);
            (void)pic_id; (void)is_idr; (void)errors;
        }
    }

    h264bsdFlushBuffer(dec);
    for (;;) {
        u32 pic_id = 0, is_idr = 0, errors = 0;
        u8 *yuv = h264bsdNextOutputPicture(dec, &pic_id, &is_idr, &errors);
        if (!yuv)
            break;
        /* flush/reorder output is consumed here as above */
    }
    return 0;
}

int main(void)
{
    storage_t dec;
    memset(&dec, 0, sizeof(dec));
    if (h264bsdInit(&dec, 1) != HANTRO_OK)
        return 1;

    /* uint8_t annexb[] = { 0,0,0,1, 0x67, ... SPS/PPS/IDR/P ... }; */
    /* decode_annexb(&dec, annexb, sizeof(annexb), 0); */

    h264bsdShutdown(&dec);
    return 0;
}
```

The reviewable C bit-writer tests and independent scalar pixel/edge models are
in `tests/transform8_cases.c`; run them with `bash task_support/run.sh target`.

## Directories

* *src* The modified source.
* *test* Contains test data available for all platforms.
* *win* Visual Studio project files and test application.
* *posix* Simple c file that loads a test file and runs through a decode loop.
* *js* Pure JavaScript version of the library created using [emscripten](http://emscripten.org/ *Note: this version is largely deprecated and replaced with the wasm version.*
* *wasm* JavaScript WebAssembly version created using [emscripten](http://emscripten.org/).
* *ios* XCode project and objective-c wrapper classes.

This project was heavily inspired by [Broadway.js](https://github.com/mbebenita/Broadway). Much love to them for pioneering the idea.

## Building

This project generally uses [rake](https://github.com/ruby/rake) as a build tool, since I find it simple, clear, and compatible with many different environments.

On Windows, you can download rake along with Ruby using [RubyInstaller](https://rubyinstaller.org)
On Mac, ruby and rake are already installed

In most cases, once you've installed the dependencies, you can build by changing to the desired directory and running:

```
rake
```

Here are very basic instructions for building each version:

### wasm and js

wasm and js use emscripten and Terser.

* Instructions for getting started with emscripten are here: https://emscripten.org/docs/getting_started/index.html
* Terser is available here: https://www.npmjs.com/package/terser (You can override Terser for another compatible uglify tool using the `UGLIFY_JS` environment variable)

### Windows

A Visual Studio project is available for the library as well as a simple test application to ensure it works.

I don't have plans to create a VSCode version of the project any time soon.

### iOS

You should only need Xcode in order to build and test the iOS version of the library. A project file for the library and a simple wrapper application is provided.

### posix

The posix build has been tested with both gcc and clang and the test application only uses POSIX.2 system calls.

### test

The test files are generated from a snippet of the movie ["Big Buck Bunny"](https://peach.blender.org) with uncompressed frames provided by [Xiph.org](https://media.xiph.org)

The encoding is done using [FFMPEG](https://ffmpeg.org)
