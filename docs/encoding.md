# Encoding

**Status: Phase 1 (intra-only) implemented and tested, plus an optional
zero-motion P-picture (inter) mode** — see [Scope](scope.md)'s Encoder
section for the full technical detail. This is the actual
`TinyMPGEncoder` API.

```cpp
#include "TinyMPGEncoder.h"

using namespace tinympg;

TinyMPGEncoder encoder;
encoder.setSize(176, 144);  // must each be a multiple of 16
encoder.setQp(8);           // 1-31, lower = higher quality/larger output

uint8_t bitstream[65536];
size_t n = encoder.encodeFrame(y, u, v, bitstream, sizeof(bitstream));
if (n == 0) {
  // encoder.lastStatus(): kInvalidSize/kInvalidQp (setSize()/setQp() never
  // called or rejected), kBufferTooSmall, or kAllocationError (inter mode)
}
// bitstream[0..n) is a complete, independently-decodable MPEG-1 elementary
// stream picture - decodable by TinyMPGDecoder, ffmpeg, or any conformant
// MPEG-1 decoder.
```

The `Allocator` template parameter is unused on the plain YUV intra-only
path (that path never allocates), but the packed-source convenience
overloads and the inter mode's reconstructed-picture buffers do allocate
through it - see [Memory](#memory-and-the-allocator-template) below.

## Every call is a complete, self-contained picture

Unlike a typical video encoder, `encodeFrame()` doesn't build up a
sequence across calls - each call writes its own `sequence_header` +
`group_of_pictures_header` + `picture_header` + slices, fully independent
of any other call. This means:

- Any single `encodeFrame()` output can be decoded entirely on its own -
  useful for the "compress one image" use case (see the root README), not
  just video.
- There's no inter-picture state to reset between unrelated pictures -
  every call is already independent.
- It costs a little more overhead per picture than a real video stream
  (which would normally send `sequence_header` once, not before every
  picture) - not optimized for streaming efficiency yet, since Phase 1's
  goal was a correct intra-only encoder first.

## Inter (P-picture) mode: `setInterMode(true)`

The default above is unchanged and remains the API's baseline. Calling
`setInterMode(true)` switches the encoder to an optional **zero-motion
P-picture sequence** mode for the "mostly static scene" case (a fixed
camera with little movement, screen capture, slides, ...):

- The first **successful** picture of a sequence is an I-picture; every
  later successful picture is a P-picture. A P-picture does no motion
  search - each macroblock predicts from the **same position** in the
  most recently *reconstructed* picture (a zero-displacement forward
  vector), and only the signed residual between the new source and that
  reference is transformed, quantized with MPEG-1's **non-intra**
  quantizer, and written as real run/level coefficients with an actual
  `coded_block_pattern`. The caller's raw source is never used as a
  reference.
- A macroblock whose six blocks all quantize to zero sends no residual:
  inside a slice it is skipped via `macroblock_address_increment` (the
  decoder reconstructs it from the inherited zero vector); the first and
  last macroblock of every slice are always coded explicitly (an
  explicit `(0,0)` motion vector, no CBP when they have no residual), so
  a slice starts and ends legally even when nothing changed.
- `sequence_header`/`group_of_pictures_header` are written **once**, only
  before the sequence's first I-picture, and `temporal_reference` counts
  successful pictures from 0, modulo 1024. The P-picture header carries
  legal zero-motion forward parameters (`full_pel_forward_vector = 0`,
  `forward_f_code = 1`).
- The encoder keeps two reconstructed-picture `Frame`s (via the
  `Allocator` template). Reconstruction runs the exact decoder path -
  non-intra dequantization, `idct8x8()`, clipping - so the reference
  matches what a conformant decoder produces; the emitted streams were
  cross-checked against real `ffmpeg` (see Verification below).

### Sequence control

The sequence restarts (next successful picture is an I-picture again,
headers resent, `temporal_reference` back to 0, held reference dropped)
on any of:

- `resetSequence()`;
- a `setInterMode()` call that actually changes the setting (toggling it
  off returns to the self-contained-I behavior, toggling it back on
  starts a fresh sequence);
- a **successful** `setSize()` whose dimensions actually change. A
  *rejected* `setSize()` (not a positive multiple of 16, over the max)
  changes nothing and does not interrupt the sequence.

`setQp()` does not restart anything - it still adjusts
`quantizer_scale` per picture, including inside a running I/P sequence.
All encode entry points (`encodeFrame`/`encodeFrameExplicit` and the
RGB888/RGB666/RGB565/YUYV overloads) share the one sequence state.

### Failure contract

On any failure - an undersized output buffer
(`kBufferTooSmall`) or an inter-picture allocation failure
(`kAllocationError`) - the call returns 0 bytes, consumes no
`temporal_reference`, and leaves the already-committed reference
untouched. Retrying with a large-enough buffer (after allocation starts
succeeding again) produces byte-identical output to an instance whose
call never failed. The reconstruction buffers allocate lazily on the
first inter-mode encode and re-allocate only after a size change.

### Scope

This is deliberately **zero-motion only**: no motion search or motion
compensation with a real vector, no B-pictures, no intra macroblocks
inside a P-picture, no rate control. A genuinely fast-moving scene gains
little, since nearly every block carries a residual anyway - its
intended regime is the low-motion one, where a still frame can collapse
to a few skipped/copied macroblocks. The continuous-frame example is
[`examples/EncodeInterSequence`](../examples/EncodeInterSequence/EncodeInterSequence.ino).

## Memory and the Allocator template

The plain YUV `encodeFrame()`/`encodeFrameExplicit()` intra-only path
still never allocates. The `Allocator` template parameter (default
`StdAllocator<uint8_t>`) backs the scratch YUV420 planes the packed
RGB/YUYV overloads convert into, and - once inter mode is enabled - the
two reconstructed-picture `Frame`s the P reference chain needs. Pass a
custom no-throw allocator (e.g. `PSRAMAllocatorESP32<uint8_t>` on ESP32)
to place them in PSRAM, exactly as on the decode side.

## Quality and compression

`quantizer_scale` (`setQp()`, 1-31, matching MPEG-1's own
`quantiser_scale_code` range) is the only quality/rate knob in Phase 1 -
no target-bitrate mode, no per-macroblock quantizer adaptation. Measured
on a real 176x144 video frame (`test/native/test_encode_iframe.cpp`'s own
correctness gate): `qscale=8` compresses to about 12% of the raw YUV420
size at ~37.5dB PSNR - broadly comparable to a mid-quality JPEG still.
Lower `qscale` values trade file size for quality; `qscale=1` is close to
lossless (limited by MPEG-1's own 8x8 DCT + quantization, not by anything
this encoder adds on top).

## Verification

Encoder output was checked against real `ffmpeg` (not just this project's
own decoder): `ffprobe` correctly identifies it as `mpeg1video` with the
right width/height/`pix_fmt`, and `ffmpeg`'s own decode matches
`TinyMPGDecoder`'s decode of the same bitstream exactly across a QP sweep
(including near-lossless `qscale=1`) and a deliberately high-frequency/
noisy synthetic pattern that forces the AC coefficient escape-coding path
(see `encoder/mpg_macroblock_encode.h`). The inter mode was checked the
same way: an I+P sequence is reported by `ffprobe` with exactly one
I-picture followed by P-pictures, and `ffmpeg`'s decoded pixels match
`TinyMPGDecoder`'s across all three planes within the expected sub-LSB
IDCT tolerance. See [Testing](testing.md#encoder) for the
`test_encode_iframe.cpp` details and why its pixel tolerance is
deliberately looser than the decoder tests'.

## Not yet implemented

No motion search/motion-compensated P encoding (the inter mode above is
zero-displacement only), no B-picture encoding, no intra macroblocks
inside P-pictures, no custom quantizer matrices, and no rate control
beyond a fixed/per-picture `setQp()` - see [Scope](scope.md)'s Encoder
section for the reasoning and what's explicitly deferred versus
permanently out of scope.
