# Memory budget

**Status: static RAM measured (native build); on-device heap numbers still
TBD** — following [TinyH264](https://github.com/pschatzmann/TinyH264)'s own
rule: real `arduino-cli`/on-device numbers only, never estimated or
fabricated figures presented as measurements. This project has no
`arduino-cli`/ESP32 hardware access in this environment yet, so the heap
(picture buffer) side of this table is still a formula, not a measurement -
only the static table sizes below come from an actual build.

## Static RAM (measured, `sizeof()` on the real tables)

The compile-time tables in `common/mpg_tables.h`/`common/mpg_vlc_tables.h`
(zig-zag scan, default intra/non-intra quantizer matrices, frame-rate/
aspect-ratio lookups, and the DC/AC/macroblock-address-increment/
macroblock-type-P/macroblock-type-B/coded-block-pattern/motion-code VLC
code tables) total **1550 bytes** of `.rodata`, measured directly via
`sizeof()` on every table in this native build - not an estimate:

| Table | Bytes |
|---|---|
| `kZigzag` | 64 |
| `kDefaultIntraMatrix` | 128 |
| `kDefaultNonIntraMatrix` | 128 |
| `kFrameRateTable` | 128 |
| `kAspectRatioTable` | 64 |
| `kDcLumCode`/`kDcLumBits` | 36 |
| `kDcChromaCode`/`kDcChromaBits` | 36 |
| `kAcCode`/`kAcBits`/`kAcRun`/`kAcLevel` | 561 |
| `kMbAddrIncCode`/`kMbAddrIncBits` | 108 |
| `kMbTypePCode`/`kMbTypePBits` (Phase B) | 21 |
| `kCbpCode`/`kCbpBits` (Phase B/C, shared) | 192 |
| `kMotionCode`/`kMotionCodeBits` (Phase B/C, shared) | 51 |
| `kMbTypeBCode`/`kMbTypeBBits` (Phase C) | 33 |

This doesn't include per-instance decoder state (a handful of ints/bools
in `Decoder<Allocator>`, negligible next to the picture buffers below), or
whatever the compiler/linker adds for alignment - a real `arduino-cli`
`.elf` size report would be the authoritative number for an actual build,
not this native-host total.

## Heap RAM (formula, not yet measured on-device)

`Frame<Allocator>`'s Y/U/V picture buffers (`common/mpg_frame.h`) are three
separate allocations per picture rather than one merged buffer, for the
same heap-fragmentation reasons TinyH264's `common/h264_frame.h` documents.
Allocation failure (out of memory) doesn't crash - it's reported as
`TinyMPGDecoder::Status::kAllocationError` - see [Decoding](decoding.md)'s
"Allocation failure handling" section and `StdAllocator.h`'s file comment
for the mechanics and why a plain `std::vector`/`std::allocator` pairing
can't do this safely on its own.
`Decoder<Allocator>` always holds **three** resident picture buffers:
`curFrame_` (the picture currently being reconstructed), `refPast_`, and
`refFuture_` (the two held reference pictures a B-picture predicts from,
clause 7.6.1 - see [Scope](scope.md)'s Phase C section and
`mpg_decoder.h`'s file comment for why two, not one) - even for a stream
that turns out to have no B-pictures at all, since Phase A/B/C share one
decode path and the decoder has no way to know in advance whether a
B-picture is coming. Each buffer is sized to `maxWidth x maxHeight`
(default `MPG_MAX_WIDTH`/`MPG_MAX_HEIGHT` = 352x288 CIF, see
`mpg_config.h`, overridable via `TinyMPGDecoder::setMaxDimension()`):

| Resolution | Y | U+V | Bytes/frame | Total (3 buffers) |
|---|---|---|---|---|
| QCIF (176x144) | 25344 | 12672 | 38016 | 114048 |
| CIF (352x288, default) | 101376 | 50688 | 152064 | 456192 |

Unlike TinyH264's default of 1 current + up to `H264_MAX_REF_FRAMES` = 3
references, MPEG-1 needs at most exactly two held references regardless of
GOP structure - two fixed, named members (`refPast_`/`refFuture_`, see
`mpg_config.h`), not a runtime-sized array - so no
`setMaxRefFrames()`-equivalent knob exists, since there's no *list* to
size, just this fixed pair. Real on-device numbers (actual heap
fragmentation behavior, `.bss`/`.data` growth from linking against
`Arduino.h`, etc.) will replace this formula once this project has
`arduino-cli`/ESP32 hardware access to measure against, the same way
TinyH264's own
[memory-budget.md](https://github.com/pschatzmann/TinyH264/blob/main/docs/memory-budget.md)
does.

## Encoder (`TinyMPGEncoder`)

Zero heap allocation on the plain YUV intra-only encode path, and no
static footprint beyond the tables above (reused as-is, not duplicated -
`encoder/mpg_macroblock_encode.h` includes
`common/mpg_tables.h`/`common/mpg_vlc_tables.h` directly rather than
carrying its own copies). In the default mode `Encoder`
(`encoder/mpg_encoder.h`) holds only a handful of `int`s
(width/height/qscale/frame-rate-code state) - a self-contained I-picture
has no spatial prediction, so that path needs no reconstructed-picture
buffer (see [Scope](scope.md)'s Encoder section for why that's
structural). `BitWriter` (`encoder/mpg_bitwriter.h`) writes directly into
the caller-provided `dst` buffer passed to `encodeFrame()` - same
never-allocate-in-the-hot-path discipline the decoder follows, just with
even less resident state to account for.

The opt-in inter mode (`setInterMode(true)`, see
[Encoding](encoding.md)) is the one exception: it lazily allocates two
full YUV420 `Frame<Allocator>` buffers through the class's `Allocator`
template parameter - the committed reference picture and the
in-progress reconstruction - i.e. twice the per-frame size quoted for the
decoder's own `Frame` above (one luma + two chroma planes each), released
again on `resetSequence()`/a size change. The packed RGB/YUYV overloads
also allocate one set of scratch YUV420 planes regardless of mode.

## Performance (measured, real hardware)

**First real measurement, not an estimate** (`double`-precision
transforms, since superseded - see below): `examples/EncodeSyntheticFrame`
on a plain ESP32 (no PSRAM, `esp32:esp32:esp32`), encoding a 176x144
(QCIF) synthetic gradient at `qscale=8` -

```
Per-frame encode time: avg=675329 us (1.5 fps), min=674778 us, max=675982 us
```

That was slow, and was expected to be at the time this was measured -
`mpg_fdct.h`/`mpg_idct.h` were, at the time of this measurement, direct,
separable, **double**-precision transforms (chosen for accuracy/
simplicity while this project's correctness was still being established,
not for speed), and the ESP32's Xtensa LX6 FPU only implements
*single*-precision (`float`) arithmetic in hardware; `double` operations
- which fdct8x8()/idct8x8() and the `quantizeAc()`/`quantizeDc()`
rounding built on top of them used throughout - fall back to slow
software emulation. At 176x144 (99 macroblocks, 594 8x8 blocks/picture),
675ms/picture works out to ~1.1ms/block, consistent with that hypothesis
(each block's forward DCT is on the order of a thousand `double`
multiply-adds).

**After the `double`-to-`float` conversion**: both files (plus
`quantizeAc()`/`quantizeDc()`) were converted to `float` throughout -
every literal deliberately `f`-suffixed (a bare floating-point literal is
`double`, and mixing one into a `float` expression silently promotes the
whole expression back to `double`, defeating the point) - per this
project's own rule that `double` must never be used on microcontroller
targets. The full native test suite, including the PSNR-based encode
round-trip test, still passes at its existing tolerances after the
conversion, so accuracy is unaffected. Re-measured on the same board
(`examples/EncodeSyntheticFrame`, same 176x144/`qscale=8` synthetic
gradient, 30 repetitions, 120 frames total):

```
Per-frame encode time: avg=86961 us (11.5 fps), min=86691 us (11.5 fps), max=87279 us (11.5 fps)
Free heap after encode: 279036 bytes
```

Avg encode time dropped from ~675ms to ~87ms - about **7.8x faster** -
confirming `double` was in fact the dominant cost here too, and the
tight min/max spread (unlike the decoder's, see below) makes sense since
the encoder does the same fixed amount of transform work every picture
(no reorder buffer, no early-release iterations to skew the average).

**Decode side, second confirming data point (double-precision, same run
as the encode figure above)**: `examples/DecodeFromProgmem` on the same
board, decoding the same 176x144 I-frame clip -

```
Per-frame decode time: avg=160559 us (6.2 fps), min=1 us, max=476227 us (2.1 fps)
```

The wide min/max spread here isn't noise - it's this decoder's own
display-order reordering (see `mpg_decoder.h`'s file comment) doing
exactly what it's designed to: only every third `write()`-loop iteration
in this particular benchmark actually decodes a new picture (`idct8x8()`,
~476ms at the time of this measurement, same ballpark as the encode
side's ~675ms, same double-precision cause) - the other two just release
an already-decoded picture already sitting in the reorder buffer, with
no transform work at all (~1us). The real per-*decoded*-picture cost was
the ~476ms figure, not the ~160ms average - which itself roughly matched
the encode side's ~675ms once you account for `idct8x8()` alone being
cheaper than `fdct8x8()` + `quantizeAc()`'s per-coefficient search
combined.

**After the `double`-to-`float` conversion**: `fdct8x8()`/`idct8x8()`
(and `quantizeAc()`/`quantizeDc()`) have been converted from `double` to
`float` (single precision, hardware-accelerated on the ESP32's FPU), per
this project's rule that `double` must never be used on microcontroller
targets - every literal deliberately `f`-suffixed (a bare floating-point
literal is `double`, and mixing one into a `float` expression silently
promotes the whole expression back to `double`, defeating the point).
The full native test suite, including the PSNR-based encode round-trip
test, still passes at its existing tolerances after the conversion, so
accuracy is unaffected. Re-measured on the same board (`examples/
DecodeFromProgmem`, same 176x144 I-frame clip, 30 repetitions, 90 frames
total):

```
Per-frame decode time: avg=34804 us (28.7 fps), min=1 us, max=90292 us (11.1 fps)
```

The real per-*decoded*-picture cost dropped from ~476ms to ~90ms - about
**5.3x faster** - confirming `double` was in fact the dominant cost, not
just a plausible hypothesis. The ~1us min (reorder-buffer-release
iterations, no transform work) is unchanged, as expected.

Both sides are now confirmed with real re-measured numbers: decode's
per-decoded-picture max went from ~476ms to ~90ms (~5.3x faster, above),
and encode's average went from ~675ms to ~87ms (~7.8x faster, see the
Encoder measurement above) - both consistent with `double` having been
the dominant cost, not just a plausible hypothesis. A fixed-point
integer implementation (matching real decoders' own approach, e.g.
FFmpeg's `simple_idct`) remains a possible future optimization beyond
`float` if a given use case still needs more throughput, but is deferred
until profiling shows it's actually needed.

### STM32H750VBT6

`examples/DecodeFromProgmem`, unmodified, on an STM32H750VBT6 (Cortex-M7
@ 480MHz) - same 176x144 I-frame clip, 30 repetitions:

```
Frame 89: 176x144, average luma=126, decode time=4390 us (227.8 fps)
Frame 90: 176x144, average luma=126, decode time=1 us (1000000.0 fps)
Decoded 90 frame(s) over 30 repetition(s) of the clip.
Per-frame decode time: avg=9595 us (104.2 fps), min=0 us, max=24909 us (40.1 fps)
Free heap after decode: (not available on this core)
```

Same reorder-buffer pattern as the ESP32/x86 numbers (a near-zero
release iteration followed by one that pays for real transform work,
here 4390us/24909us depending on position in the loop) - the real
per-decoded-picture cost is the **24909 us (40.1 fps) max**, not the
9595us average. Faster than the ESP32 (40.1 fps vs 11.1 fps max) despite
a similar-generation Cortex-M core, consistent with the M7's higher
480MHz clock and single-cycle hardware FPU. `ESP.getFreeHeap()` has no
STM32 equivalent, so free-heap reporting is unavailable on this core (see
`DecodeFromProgmem`'s own `printFreeHeap()` fallback) - no heap number to
report here.

`examples/EncodeSyntheticFrame`, unmodified, on the same board, same
176x144/`qscale=8` synthetic gradient, 30 repetitions -

```
Encoded 120 frame(s) over 30 repetition(s) of the sequence.
Per-frame encode time: avg=22943 us (43.6 fps), min=22739 us (44.0 fps), max=23134 us (43.2 fps)
Free heap after encode: (not available on this core)
```

Tight min/max spread, same as the ESP32/x86 encode-side numbers above -
expected, since the encoder does the same fixed amount of transform work
every picture (no reorder buffer to skew individual frames). Faster than
the ESP32 (43.6 fps vs 11.5 fps avg), consistent with the same clock/FPU
advantage seen on the decode side.

### x86 (desktop) numbers

All the numbers above are ESP32-only. `test/native/bench_native.cpp` is
the desktop counterpart - same embedded clip
(`examples/DecodeFromProgmem/mpg_test_clip.h`) and same synthetic
gradient/repetition counts as `examples/EncodeSyntheticFrame`, just timed
with `std::chrono::steady_clock` instead of Arduino's `micros()`. It's
not a correctness test (no assertions), so it isn't part of `ctest` -
build/run it manually:

```
cmake --build build --target bench_native
./build/test/native/bench_native
```

Measured on the development machine (Intel Core i7-4650U @ 1.70GHz,
x86_64 Linux, g++ 13.3.0, `-O2`), `float` transforms (post-conversion):

```
Decoded 90 frame(s) over 30 repetition(s) of the clip.
Per-frame decode time: avg=517 us (1934.2 fps), min=0 us, max=2149 us (465.3 fps)
Encoded 120 frame(s) over 30 repetition(s) of the sequence.
Per-frame encode time: avg=1846 us (541.7 fps), min=1092 us (915.8 fps), max=3371 us (296.6 fps)
```

Both are two to three orders of magnitude faster than the ESP32 figures
above (decode avg ~35ms → ~0.5ms, encode avg ~87ms → ~1.8ms) - expected,
given a desktop x86 CPU clocked several times higher than the ESP32's
240MHz Xtensa LX6, with full hardware `double` support (irrelevant here
since this codebase no longer uses `double`, see above) and generally
far more capable branch prediction/cache/pipelining for this kind of
scalar per-block work. Like the ESP32 numbers, this reflects only one
specific desktop CPU - re-run `bench_native` locally for your own
machine's numbers rather than assuming these transfer.

### Impact of picture type (I vs. P/B) on these numbers

Both benchmarks above measure **I-frame-only** streams, which is the
worst case for this codec, not a representative one:

- `examples/EncodeSyntheticFrame` only ever produces I-frames - it leaves
  the encoder at its intra-only default (it doesn't call
  `setInterMode(true)`, see [Encoding](encoding.md)). The optional
  zero-motion P mode (`examples/EncodeInterSequence`) can make a
  low-motion frame far cheaper than an I-frame (skipped/copied
  macroblocks carry few or no coefficients), but it is not a
  general motion-search encoder, so the all-I figure below remains the
  conservative worst case to budget for.
- `examples/DecodeFromProgmem`'s embedded clip is also all I-frames -
  not a deliberate choice to test the worst case, just an artifact of
  its flat-gray test content giving `ffmpeg` no residual worth coding as
  a P-frame at the short encode duration used (see
  `mpg_test_clip.h`'s own comment).

An I-frame decodes/encodes all 6 8x8 blocks of every macroblock through
the (I)DCT - the dominant per-picture cost demonstrated above. A
P-picture (Phase B, already implemented - see
[Scope](scope.md)'s Phase B section) can skip that entirely for
macroblocks that are motion-compensated with no residual ("not coded")
or motion-compensated-only, and B-pictures (Phase C) have the same
skip/motion-only paths available. On real video content with a normal
GOP structure, this means P/B-picture decode is architecturally expected
to be faster than the I-frame figures above, sometimes considerably so
depending on how much of the frame is actually changing. This hasn't
been measured on hardware yet, though - no P/B-frame timing number is
given here, since this project only reports real measurements, not
architectural expectations presented as one.
