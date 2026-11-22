# Testing

Desktop-only native test suite (`test/native/`), following
[TinyH264](https://github.com/pschatzmann/TinyH264)'s exact approach: each
`test_*.cpp` is a standalone executable, and the five that touch a real
bitstream (`test_headers`, `test_decode_iframe`, `test_decode_pframe`,
`test_decode_bframe`, `test_encode_iframe`) decode and/or encode a real
`ffmpeg`-generated/verified MPEG-1 stream and check the result against
`ffmpeg`'s own decode — this project's correctness oracle, not
conformance-suite guesswork. None of this runs on an embedded target; it's
a host-only (native g++/clang/MSVC) build against the same headers
`examples/` will use.

**Status: implemented, Phase A/B/C (decoder) and Phase 1 (encoder)
passing.** `test/native/CMakeLists.txt`
isn't a standalone CMake project of its own (no `project()`/
`cmake_minimum_required()` - it expects the `TinyMPG` INTERFACE target to
already exist, the same way TinyH264's does) - build from the repository
root, which detects it's the top-level project and pulls this directory
in via `add_subdirectory()` (see the root `CMakeLists.txt`):

```sh
mkdir build && cd build
cmake ..
make
ctest --output-on-failure
```

Or, as a CMake subproject from a parent build: `add_subdirectory(TinyMPG)`
then `target_link_libraries(your_target PRIVATE TinyMPG)`.

For ESP-IDF: reference this directory via `EXTRA_COMPONENT_DIRS` (the
top-level `CMakeLists.txt` already handles the `ESP_PLATFORM` branch via
`idf_component_register()`).

## Why pixel comparisons are tolerance-based, not byte-exact

`test_decode_iframe.cpp` allows each decoded sample to differ from
`ffmpeg`'s own decode by a small bound (currently: never more than 2 per
sample, and a whole-frame average under 0.2) rather than requiring literal
byte-for-byte equality. This isn't a weakened test - it's simply accurate
to what ISO/IEC 11172-2 actually normative: the DCT coefficient VLC
tables, dequantization arithmetic, and zig-zag placement are all fully
determined by the spec (and *are* verified exactly - see
`mpg_macroblock.h`'s file comment for how its dequantization formula was
checked bit-for-bit against `ffmpeg -debug 64`'s own internal coefficient
dump), but the 8x8 IDCT itself is only held to an accuracy bound, not a
specific algorithm - `mpg_idct.h` is a direct double-precision
implementation of the textbook DCT-III formula, not a port of `ffmpeg`'s
own fixed-point `simple_idct`, so a handful of samples landing 1 LSB
apart from `ffmpeg`'s particular rounding is expected on real content.
`test_decode_pframe.cpp` uses a slightly looser bound (never more than 4
per sample, whole-frame average under 0.3) for the same underlying reason,
just compounded slightly: each P-picture predicts from the *previously
decoded* picture, so a sub-LSB rounding difference from one picture can
carry into the next rather than resetting every frame the way Phase A's
all-intra stream does. `test_decode_bframe.cpp` reuses that same P-picture
bound (max 4/sample, mean under 0.3) rather than tightening or loosening
it further: a B-picture's own rounding error doesn't propagate forward
(nothing predicts from a B-picture), but it does compound *two* directions'
worth of sub-LSB IDCT drift from its two anchor pictures in one averaged
prediction, which roughly offsets the lack of forward propagation.

## How test assets are generated

See [Preparing input with ffmpeg](preparing-input-with-ffmpeg.md) for the
exact `ffmpeg` commands used to produce `test/native/assets/*.m1v` (input)
and `*.yuv` (oracle decode output) - regenerate them with those same
commands if the test clips ever need to change, rather than hand-editing
the binary assets.

## Test list (Phase A/B/C)

Mirroring TinyH264's explicit-list-not-glob convention in
`test/native/CMakeLists.txt` (a glob silently misses newly-added test files
without a manual re-configure):

- `test_bitreader` - `mpg_bitreader.h`'s `u(n)` reads, byte-alignment, and
  the generic `readVlc()` table walk, independent of any real bitstream.
- `test_tables` - VLC/zigzag/quantizer-matrix table sanity (structural
  checks, e.g. every VLC table entry decodes to a unique, round-trippable
  symbol) - mirrors TinyH264's `test_cavlc_tables.cpp`.
- `test_headers` - sequence/picture header parsing against
  `assets/small.m1v` (176x144, cross-checked by hand against the raw
  bitstream bytes).
- `test_decode_iframe` - I-frame decode of `assets/small.m1v` (10 frames,
  all I-pictures) vs. `ffmpeg`'s own decode of the same file - the core
  correctness gate for Phase A. Tolerance-based, not byte-exact - see
  above.
- `test_decode_pframe` - I+P-frame decode of `assets/pframe.m1v` (10
  frames: 1 I-picture + 9 P-pictures) vs. `ffmpeg`'s own decode - the core
  correctness gate for Phase B (motion vector decode, half-pel motion
  compensation, inter block dequantization, skipped-macroblock handling).
  Also see `docs/scope.md` for the P-picture-before-any-reference error
  path, which isn't covered by pixel-diffing a valid stream and would need
  a deliberately malformed asset to exercise directly - not yet automated.
- `test_decode_bframe` - I+P+B-frame decode of `assets/bframe.m1v` (a
  repeating I,P,B,B GOP; 10 pictures in *decode* order, delivered in
  *display* order I,B,B,P,B,B,P,B,B,I) vs. `ffmpeg`'s own decode - the
  core correctness gate for Phase C (backward and bidirectional motion
  vector decode, bidirectional half-pel averaging, B-picture skip's
  mode-inheritance semantics, and the display-order output reordering
  itself - the assertions on frame count/order would fail if reordering
  lost or duplicated a picture, even before any pixel comparison runs).
  Also exercises the two-reference (`refPast_`/`refFuture_`) bookkeeping
  in `mpg_decoder.h` end to end.
- `test_alloc_failure` - the one test that doesn't touch a real bitstream
  fixture at all (beyond `assets/small.m1v` for its one success-path
  check); it plugs in a mock `Allocator` whose `allocate()` fails on
  demand and checks that `TinyMPGDecoder` reports
  `Status::kAllocationError` cleanly - no crash - whether the failure
  happens on the very first picture-buffer allocation, on a later one, or
  via `begin()`'s eager allocation path, and that a decoder recovers once
  allocation starts succeeding again. See `StdAllocator.h`'s file comment
  for why this needed its own `Buffer<Allocator>` (`common/mpg_frame.h`)
  rather than continuing to rely on `std::vector`.

## Encoder

- `test_encode_iframe` - encodes a real decoded video frame
  (`assets/small_ref.yuv`'s first frame, reused here as encoder *input*
  rather than a decode oracle) with `TinyMPGEncoder`, decodes the result
  back with `TinyMPGDecoder`, and checks the round-trip against the
  original source - the core correctness gate for the intra-only encoder
  (see [Scope](scope.md)'s Encoder section). Its tolerance is deliberately
  much looser than the decoder tests' (a PSNR floor, not just a sample-
  difference ceiling - see the test file's own comment for why: this test
  goes through real, lossy quantization, which the decoder tests never
  have to absorb since they diff against `ffmpeg`'s own decode of an
  already-encoded stream). Development-time-only verification beyond what
  this test re-runs: `ffprobe`/`ffmpeg` cross-checks (correct codec/
  dimensions, matching decode across a QP sweep including near-lossless
  `qscale=1`, and a synthetic high-frequency stress pattern exercising the
  AC coefficient escape-coding path) - see [Encoding](encoding.md).
