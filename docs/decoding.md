# Decoding

**Status: Phase A/B/C (I-, P-, and B-frames) implemented and tested** — this
is the actual `TinyMPGDecoder<Allocator>` API, designed to closely match
[TinyH264](https://github.com/pschatzmann/TinyH264)'s `TinyH264Decoder<Allocator>`
so callers already familiar with one can use the other with minimal
relearning. See [Scope](scope.md) for exactly what decodes today (I-, P-,
and B-pictures; D-pictures are the only kind still unsupported). No public
API shape changes were needed for either P- or B-picture support -
`write()`/`decodeNext()` already report `kUnsupported` for a feature this
decoder doesn't implement, which now means D-pictures specifically rather
than any non-I picture - but B-picture support does change *when*
`onFrame()` fires relative to the raw bitstream, see below.

## Frame delivery order and latency

`onFrame()` fires once per picture in **display** order, not the
bitstream's decode order - a B-picture is transmitted (and decoded) after
both of the anchor (I/P) pictures it depends on, but displays *between*
them (ISO/IEC 11172-2 clause 2.4.3.6). To present pictures in the right
order, `TinyMPGDecoder` holds back each anchor picture until it can be
sure no B-picture still needs to be delivered ahead of it - concretely,
delivery of the callback for a given anchor picture is delayed until the
*next* anchor picture (or B-pictures depending on it) has been decoded,
with a final flush releasing the last anchor once no more start codes
remain in the input. This means:

- For a stream with no B-pictures, delivery is still delayed by one
  anchor's worth of decoding, and the very last picture in the stream is
  only delivered once `write()` sees end-of-input (or a subsequent
  `write()` call supplies no further picture start codes) - the total
  frame count and content are unaffected, only the timing of the last
  callback within/across `write()` calls.
- For a stream with B-pictures, the callback order matches what a video
  player would actually show on screen (I, B, B, P, B, B, P, ...), not
  the order pictures appear in the raw elementary stream (I, P, B, B, P,
  B, B, ...).

See `mpg_decoder.h`'s file comment for the full reference-management
mechanics (`refPast_`/`refFuture_`, swap-based promotion) behind this.

## Intended usage

```cpp
#include "TinyMPGDecoder.h"
using namespace tinympg;

void onFrame(TinyMPGDecoder<> &decoder, void *userData) {
  // decoder.width()/height()/y()/u()/v()/strideY()/strideUV()
  // or decoder.toRGB565(dst, dstCapacity) / toYUV420(dst, dstCapacity)
}

TinyMPGDecoder<> decoder;
decoder.setCallback(onFrame);
decoder.begin();
decoder.write(mpeg1EsData, size);  // one or more picture_start_code-delimited
                                    // access units; onFrame() runs once per
                                    // decoded picture before write() returns
if (decoder.hasError()) { /* bitstream error, or an unsupported feature
                              (e.g. a D-frame) */ }
```

Feeding it from `arduino-audio-tools`' `DemuxerMPG` (`ContainerMPG.h`):

```cpp
DemuxerMPG demuxer;
TinyMPGDecoder<> decoder;
// MPGDecoder (arduino-audio-tools' AudioTools/Video/CodecMPG.h) wraps
// TinyMPGDecoder as demuxer's video sink, mirroring how CodecH264.h wraps
// TinyH264Decoder
demuxer.setOutputVideo(mpgDecoderWrapper);
demuxer.begin();
demuxer.write(mpgProgramStreamData, len);
```

## Streaming/incremental input

`write(data, size, isLastChunk)`'s third parameter defaults to `true`,
matching the usage above: `data` is treated as one self-contained buffer
(a whole clip, or at least whole pictures, per call), and reaching the
end of it while a picture_header or slice is still open is taken to mean
that unit is complete - correct for a buffer with nothing more coming
after it.

That assumption breaks for a real container-demuxed stream fed through
in chunks that don't line up with picture/slice boundaries - concretely,
`DemuxerMPG` forwards unbounded video PES data to its video sink in
whatever increments happen to be buffered, not one whole access unit at
a time (see `ContainerMPG.h`'s `continuePayload()`). Feeding that through
`write()` with the default `isLastChunk=true` doesn't fail cleanly: it
can silently reconstruct a picture with whole rows of stale macroblocks
where its true final slice got cut off mid-buffer, and interacts
especially badly with `setIgnorePFrames()` (see "Skipping P-pictures"
below), which depends on cleanly parsing a picture_header to decide
whether to skip it.

Pass `isLastChunk=false` for every chunk of such a stream except
(optionally) the very last one:

```cpp
// data/len is whatever DemuxerMPG happened to buffer for this call - not
// necessarily a whole picture.
decoder.write(data, len, /*isLastChunk=*/false);
```

A unit that can't yet be proven complete is held internally (not
processed, not discarded) and completed by whatever later `write()` call
provides the rest of its bytes, however many calls that takes -
`onFrame()` still only fires once a picture is genuinely fully decoded.
This can fail with `Status::kAllocationError` if internally carrying
those bytes over requires an allocation that fails (see "Allocation
failure handling" below) - check `hasError()` the same way as any other
`write()` call. See `mpg_decoder.h`'s "Streaming/incremental input" file
comment for the full mechanics.

## Skipping P-pictures

`decoder.setIgnorePFrames(true)` discards every P-picture as soon as its
picture_header is parsed - none of its slices are decoded, no `onFrame()`
callback fires for it, and it's silently dropped rather than reported as
an error. I- and B-pictures are unaffected. Off by default.

```cpp
decoder.setIgnorePFrames(true);  // decode only I- (and, if present, B-)pictures
```

Two things worth knowing before reaching for this:

- A P-picture chained off an ignored one, or a B-picture that would have
  referenced it, ends up predicting from whichever anchor is still held
  instead - an accepted consequence of skipping frames this way, not a
  bug, but it means picture quality/timing can look different from just
  "the same video with fewer frames."
- It's fed by the same `write()` calls as everything else, so the
  streaming-input caveat above applies here too - in fact this is
  exactly the combination (real DemuxerMPG-fed input, `isLastChunk`
  defaulted to `true`) that surfaced the streaming-input bug in the
  first place, since a mis-parsed, boundary-split picture_header can
  make this feature skip - or fail to skip - the wrong picture.

## Accessing pixel data

Same shape as `TinyH264Decoder` (see that project's
[Decoding](https://github.com/pschatzmann/TinyH264/blob/main/docs/decoding.md)
doc for the full rationale behind each conversion):

- `y()`/`u()`/`v()` + `strideY()`/`strideUV()`: raw YUV 4:2:0 planar access,
  no copying.
- `toRGB565(dst, dstCapacity)` / `toRGB666(...)` / `toRGB888(...)`: packed
  RGB conversion for common embedded display libraries.
- `toYUV420(dst, dstCapacity)`: tightly-packed I420 copy (Y, then U, then
  V, no row padding) for handing a whole frame to storage/network code.
- Windowed/tiled overloads of the above (`x, y, dx, dy` variants), for
  pushing a decoded picture to a display in tiles instead of all at once.

The `Allocator` template parameter (default `StdAllocator<uint8_t>`, see
`StdAllocator.h`) controls where the picture buffer(s) live — pass
`PSRAMAllocatorESP32<uint8_t>` on boards with PSRAM (e.g. ESP32-S3), the
same pattern as `TinyH264Decoder<PSRAMAllocatorESP32<uint8_t>>`.

## Allocation failure handling

Unlike the STL's own `std::allocator`, `StdAllocator` (this library's
default) never throws `std::bad_alloc` on an out-of-memory condition - it
returns `nullptr`, and every allocation call site in this library checks
for that instead of assuming it can't happen. This matters on a
microcontroller for two reasons: many embedded C++ toolchains build with
exceptions disabled entirely (a thrown `std::bad_alloc` would call
`std::terminate()`/abort the process outright, regardless of any
`try`/`catch`), and even with exceptions enabled, this library's own
code never used to catch such a throw - it would propagate out uncaught,
which is also a crash. Instead, an allocation failure - `begin()`'s
up-front allocation, or the lazy allocation the first `write()` call
triggers - surfaces as `TinyMPGDecoder::Status::kAllocationError`
(`hasError()` returns true for it, same as `kError`/`kUnsupported`) so
the caller can detect and handle it like any other decode failure,
rather than the process going down. `PSRAMAllocatorESP32` follows the
same no-throw contract; a custom `Allocator` should too if you supply
your own (see `StdAllocator.h`'s file comment for the full rationale,
including why a plain `std::vector` can't safely detect this on its
own).

`width()`/`height()` are the stream's *display* dimensions
(`horizontal_size`/`vertical_size`, not necessarily a multiple of 16) —
unlike `TinyH264Decoder`, TinyMPG's internal picture buffer is sized to
the macroblock-aligned *coded* dimensions and crops to the display size
automatically in every accessor and conversion method, since MPEG-1
(unlike H.264 baseline as TinyH264 implements it) permits a coded
resolution that isn't itself a multiple of 16.
