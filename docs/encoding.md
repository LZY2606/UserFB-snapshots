# Encoding

**Status: Phase 1 (intra-only) implemented and tested** — see
[Scope](scope.md)'s Encoder section for the full technical detail. This is
the actual `TinyMPGEncoder` API.

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
  // called or rejected) or kBufferTooSmall
}
// bitstream[0..n) is a complete, independently-decodable MPEG-1 elementary
// stream picture - decodable by TinyMPGDecoder, ffmpeg, or any conformant
// MPEG-1 decoder.
```

No `Allocator` template parameter, unlike `TinyMPGDecoder<Allocator>` -
`TinyMPGEncoder` holds no picture buffer at all (see
[Scope](scope.md)'s Encoder section for why an intra-only encoder needs
none) and so has nothing for an allocator to place.

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
(see `encoder/mpg_macroblock_encode.h`). See
[Testing](testing.md#encoder) for the `test_encode_iframe.cpp` details and
why its pixel tolerance is deliberately looser than the decoder tests'.

## Not yet implemented

No motion estimation (P/B-picture encoding), no custom quantizer matrices,
no rate control beyond a fixed `qscale` - see [Scope](scope.md)'s Encoder
section for the reasoning (why intra-only at all) and what's explicitly
deferred versus permanently out of scope.
