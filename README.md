# Low Complexity Communication Codec (LC3)

LC3 and LC3 Plus are audio codecs designed for low-latency audio transport.

- LC3 is specified by [_the Bluetooth Special Interset Group for the LE Audio
  protocol_](https://www.bluetooth.org/DocMan/handlers/DownloadDoc.ashx?doc_id=502107&vId=542963)

- LC3 Plus is defined by [_ETSI TS 103 634_](https://www.etsi.org/deliver/etsi_ts/103600_103699/103634/01.04.01_60/ts_103634v010401p.pdf)

In addition to LC3, following features of LC3 Plus are proposed:
- Frame duration of 2.5 and 5ms.
- High-Resolution mode, 48 KHz, and 96 kHz sampling rates.

## Overview

The directory layout is as follows :
- include:      Library interface
- src:          Source files
- tools:        Standalone encoder/decoder tools
- python:       Python wrapper
- test:         Unit testing framework
- fuzz:         Roundtrip fuzz testing harness
- build:        Building outputs
- bin:          Compilation output

## How to build

The default toolchain used is GCC. Invoke `make` to build the library.

```sh
$ make -j
```

Compiled library `liblc3.so` will be found in `bin` directory.

LC3 Plus features can be selectively disabled :
- `LC3_PLUS=0` disable the support of 2.5ms and 5ms frame durations.
- `LC3_PLUS_HR=0` turns off the support of the High-Resolution mode.

Only Bluetooth LC3 features will be included using the following command:

```sh
$ make LC3_PLUS=0 LC3_PLUS_HR=0 -j
```

#### Cross compilation

The cc, as, ld and ar can be selected with respective Makefile variables `CC`,
`AS`, `LD` and `AR`. The `AS` and `LD` selections are optionnal, and fallback
to `CC` selection when not defined.

The `LIBC` must be set to `bionic` for android cross-compilation. This switch
prevent link with `pthread` and `rt` libraries, that is included in the
bionic libc.

Following example build for android, using NDK toolset.

```sh
$ make -j CC=path_to_android_ndk_prebuilt/toolchain-prefix-clang LIBC=bionic
```

Compiled library will be found in `bin` directory.

#### Web Assembly (WASM)

Web assembly compilation is supported using LLVM WebAssembly backend.
Installation of LLVM compiler and linker is needed:

```sh
# apt install clang lld
```

The webasm object is compiled using:
```sh
$ make CC="clang --target=wasm32"
```

## Tools

Tools can be all compiled, while invoking `make` as follows :

```sh
$ make tools
```

The standalone encoder `elc3` take a `wave` file as input and encode it
according given parameter. The LC3 binary file format used is the non
standard format described by the reference encoder / decoder tools.
The standalone decoder `dlc3` do the inverse operation.

Refer to `elc3 -h` or `dlc3 -h` for options.

Note that `elc3` output bitstream to standard output when output file is
omitted. On the other side `dlc3` read from standard input when input output
file are omitted.
In such way you can easly test encoding / decoding loop with :

```sh
$ alias elc3="LD_LIBRARY_PATH=`pwd`/bin `pwd`/bin/elc3"
$ alias dlc3="LD_LIBRARY_PATH=`pwd`/bin `pwd`/bin/dlc3"
$ elc3 <in.wav> -b <bitrate> | dlc3 > <out.wav>
```

Adding Linux `aplay` tools, you will be able to instant hear the result :

```sh
$ alias elc3="LD_LIBRARY_PATH=`pwd`/bin `pwd`/bin/elc3"
$ alias dlc3="LD_LIBRARY_PATH=`pwd`/bin `pwd`/bin/dlc3"
$ elc3 <in.wav> -b <bitrate> | dlc3 | aplay -D pipewire
```

## Repair window

The repair window (`include/lc3_repair.h`) is a bounded decode window for
pre-roll preview with late packet correction. A frame first entered as lost
(PLC) can be replaced once the original packet arrives, together with any
later frame still held, without ever advancing a decoder permanently during
a preview.

```c
lc3_repair_t *w = lc3_repair_create(
    10000 /* dt_us */, 32000 /* sr_hz */, 48000 /* sr_pcm_hz, 0 follows */,
    2 /* nchannels */, 8 /* capacity, 2 to 32 frames */);

uint64_t id;
struct lc3_repair_frame frames[2] = {
    { packet_ch0, nbytes }, { NULL, 0 }   /* channel 1 is lost */
};
lc3_repair_append(w, frames, &id);       /* id starts at 0, consecutive */

int16_t preview[ns * 2];
lc3_repair_preview(w, id, preview);      /* whole interleaved frame */

frames[1] = (struct lc3_repair_frame){ late_packet, nbytes };
lc3_repair_replace(w, id, frames);       /* correction before playback */

int16_t pcm[capacity_samples * 2];
int n = lc3_repair_read(w, pcm, capacity_samples);
```

Units

- Frame sizes (`nbytes`) are per channel, in bytes. A NULL `data` pointer
  with `nbytes == 0` marks a lost channel; any other payload must lie in the
  frame byte range of the configuration (20 to 400 bytes for standard LC3).
- `capacity`, frame identifiers and the frames array count frames; the
  window holds 2 to 32 frames.
- `lc3_repair_read()` and its `capacity_samples` argument count PCM samples
  **per channel**. One frame contains `lc3_frame_samples(dt_us, sr_pcm_hz)`
  samples per channel, and the interleaved S16 output holds
  `nchannels` times that many `int16_t` values.

Repair range

- Any frame still held by the window whose playback has not started can be
  replaced, repeatedly and out of order. The frame and all its successors
  are recomputed from decoder state snapshots, giving exactly the sound of
  decoding the final packet sequence in order with ordinary `lc3_decode()`,
  including independent per-channel losses, consecutive PLC and loss after
  recovery.
- Previews are not playback: they target any retained frame in any order,
  any number of times, and never consume capacity.
- As soon as one sample of a frame is read, that frame can no longer be
  replaced; its remaining PCM is still delivered in full, and the slot is
  recycled only once the whole frame has been read. Fully read or reset
  frames cannot be addressed through their old identifiers.
- Retained memory grows only with the codec configuration, the channel
  count and the capacity; played packets and frames are never stored, and
  decoding does not restart at frame 0 while the window moves forward.
  Snapshots are in-memory only, for a single process and build.

Return values

- `lc3_repair_create()` returns a handle, or NULL on invalid parameters or
  allocation failure.
- `lc3_repair_append()`, `lc3_repair_replace()` and `lc3_repair_preview()`
  return 0 on success and -1 on failure (invalid parameters, full window, or
  an unknown / no longer replaceable frame). Failures are atomic: no state,
  frame numbering, retained audio or output parameter is modified. A
  legal-size packet rejected by the decoder is accepted and rendered as
  PLC, exactly as `lc3_decode()` does.
- `lc3_repair_read()` returns the number of samples per channel written
  (0 when the window is temporarily empty); `lc3_repair_reset()` returns
  nothing and clears all history while keeping frame numbering monotonic.

The supported configurations are standard LC3 frame durations of 7.5 and
10 ms, stream rates of 8/16/24/32/48 kHz, one or two channels, and an output
rate (`sr_pcm_hz`, 0 to follow the stream rate) greater than or equal to the
stream rate. Output PCM is always interleaved signed 16 bits.

## Test

A python implementation of the encoder is provided in `test` diretory.
The C implementation is unitary validated against this implementation and
intermediate values given in Appendix C of the LC3 specification.

#### Prerequisite

```sh
# apt install python3 python3-dev python3-pip
$ pip3 install scipy numpy
```

#### Running test suite

```sh
$ make test
```

## Fuzzing

Roundtrip fuzz testing harness is available in `fuzz` directory.
LLVM `clang` and `clang++` compilers are needed to run fuzzing.

The encoder and decoder fuzzers can be run, for 1 million iterations, using
target respectively `dfuzz` and `efuzz`. The `fuzz` target runs both.

```sh
$ make efuzz    # Run encoder fuzzer for 1M iteration
$ make dfuzz    # Run decoder fuzzer for 1M iteration
$ make fuzz -j  # Run encoder and decoder fuzzers in parallel
```

## Qualification / Conformance

The implementation is qualified under the [_QDID 194161_](https://launchstudio.bluetooth.com/ListingDetails/160904) as part of Google Fluoride 1.5.

The conformance reports can be found [here](conformance/README.md)

## Listening Test

The codec was [_here_](https://hydrogenaud.io/index.php/topic,122575.0.html)
subjectively evaluated in a blind listening test.


## Meson build system

Meson build system is also available to build and install lc3 codec in Linux
environment.

```sh
$ meson setup build
$ cd build && meson install
```

## Python wrapper

A python wrapper, installed as follows, is available in the `python` directory.

```sh
$ python3 -m pip install .
```

Decoding and encoding tools are provided in `python/tools`, like C tools,
you can easly test encoding / decoding loop with :

```sh
$ python3 ./python/tools/encoder.py <in.wav> --bitrate <bitrate> | \
  python3 ./python/tools/decoder.py > <out.wav>
```
