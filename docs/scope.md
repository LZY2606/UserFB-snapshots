# Scope

Following [TinyH264](https://github.com/pschatzmann/TinyH264)'s own
`docs/scope.md` precedent: explicitly separate what's implemented and
validated from what's deliberately deferred, rather than leaving gaps
undocumented.

**Phase A, Phase B, Phase C (decoder), and the Phase 1 intra-only encoder
are implemented and passing their native test suite** (see
[Testing](testing.md)) — the rest of this file describes the target scope
for each phase; check back (or `git log`) for what else has landed since.

## Phase A (decoder, implemented) — I-frames only

- Sequence header, group-of-pictures header, and picture header parsing.
  "Constrained parameters" MPEG-1 only: progressive, 4:2:0 — MPEG-1 has no
  interlace or chroma-format flag to worry about in the first place (unlike
  H.264/MPEG-2), which simplifies this considerably.
- **Intra (I-frame) macroblock decode only**: DCT coefficient VLC decode
  (the differentially-coded DC term per spec, plus the AC run/level table
  including its escape code), dequantization against the intra quantizer
  matrix, 8x8 IDCT, direct pixel writeback.
  - Notably simpler than H.264 intra: MPEG-1 has **no spatial
    intra-prediction modes** (no I_4x4/I_16x16 direction choice) — each
    block decodes independently except for the DC predictor, which is
    carried differentially across intra macroblocks in coding order.
- Pixel output: `y()`/`u()`/`v()` planar accessors, plus `toRGB565()`/
  `toRGB666()`/`toRGB888()`/`toYUV420()` conversion — same API shape and
  underlying colorspace math as `TinyH264Decoder` (`mpg_rgb.h` is a direct
  port of TinyH264's `h264_rgb.h`; the YUV->RGB math itself is
  codec-agnostic).
- A stream containing a D-frame `picture_coding_type` value (or a P/B-frame
  arriving before the reference picture(s) it needs have been decoded) is
  detected and reported as unsupported/an error
  (`DecodeStatus::kUnsupported`/`kError`) rather than mis-decoded — same
  "detect and reject, don't guess" policy TinyH264 applies to
  CABAC/B-slices/etc.

## Phase B (decoder, implemented) — P-frames

- Forward-only motion compensation: half-pel luma/chroma interpolation
  (bilinear — MPEG-1's motion compensation is simpler than H.264's 6-tap
  luma filter). P-pictures may freely mix intra- and inter-coded
  macroblocks (clause 2.4.3.7) - an intra macroblock inside a P-picture
  reuses Phase A's own intra decode path unchanged.
- Motion vector VLC decode (`motion_code`/`motion_residual`), differentially
  coded per macroblock, with the "modulo decoding" wraparound (clause
  7.6.3.1) that lets a motion vector legally point a few pixels past the
  picture edge - handled by clamping (border replication) at the sample
  level, see `mpg_motion.h`.
- `coded_block_pattern` VLC decode, and skipped-macroblock handling
  (P-macroblocks that carry no residual and reuse the *previous coded
  macroblock's* motion vector, not necessarily the differential-coding
  predictor - the two are tracked separately, see
  `decodePMacroblock()`'s comment in `mpg_macroblock.h`).
- Needs exactly one held reference picture (the most recently decoded I- or
  P-frame) — no reference-picture-list machinery like H.264's, MPEG-1
  P-frames always reference the single preceding reference picture (see
  `mpg_config.h`).
- As with Phase A's intra dequantization shift, the non-intra (inter)
  block dequantization shift needed empirical correction against a real
  P-picture bitstream rather than trusting FFmpeg's current source
  literally - see `mpg_macroblock.h`'s file comment.

## Phase C (decoder, implemented) — B-frames

- Bidirectional motion compensation: forward-only, backward-only, or
  averaged (bidirectional) prediction per macroblock, clause 7.6.5. The
  averaging step is a plain round-half-up 2-tap blend applied *after* the
  first direction is written (`predictBlockHalfPelAvg()`, `mpg_motion.h`)
  rather than one combined formula - this specific "put forward, then avg
  backward on top" sequencing (not the reverse, and not a single average
  of two independently-computed predictions) is what MPEG-1 actually
  specifies, and getting it backwards or combined differently would
  produce a *different* (still plausible-looking) result - see that
  file's comment. B-pictures may also freely mix intra-coded macroblocks
  in with forward/backward/bidirectional ones (same as P-pictures can
  mix in intra, clause 2.4.3.7).
- Motion vector VLC decode is shared unchanged with Phase B
  (`decodeMotionComponent()`) - a B-picture's forward and backward
  vectors each get their own independent differential predictor and
  `f_code` (`forward_f_code`/`backward_f_code`, both from
  `picture_header()`).
- `coded_block_pattern` VLC decode is likewise shared unchanged. Skipped-
  macroblock handling is *not* shared with Phase B, though: a P-picture's
  skip is always unconditionally forward-only, but a B-picture's skip
  reuses the *entire prediction mode* (forward-only/backward-only/
  bidirectional, plus whichever motion vector(s) that implies) of the
  most recently decoded real macroblock, clause 7.6.6 - see
  `decodeBMacroblock()`'s comment in `mpg_macroblock.h`. A skip
  immediately after an intra macroblock is rejected as a bitstream error
  (there's no meaningful motion to inherit), matching FFmpeg's own
  "skip with previntra" check.
- Needs **two** held reference pictures (the anchor - I or P - picture
  immediately before and after this B-picture in *display* order,
  clause 7.6.1) rather than Phase B's one, managed via `std::swap()`
  (an O(1) pointer exchange, not a ~150KB+ memcpy) the same way TinyH264
  manages its own reference list - see `mpg_decoder.h`'s file comment.
- **Output reordering**: a B-picture is transmitted (and decoded) *after*
  both of the reference pictures it depends on, even though it displays
  *between* them (clause 2.4.3.6's note on `temporal_reference`) - so
  `Decoder::next()` delivers pictures to the caller in *display* order,
  not decode order, which unavoidably delays every anchor (I/P) picture's
  delivery by one further anchor's worth of decoding (including the very
  last anchor of a stream, flushed once no more start codes remain) -
  see `mpg_decoder.h`'s file comment for the full mechanics. This delay
  is applied uniformly, even to a stream that turns out to have no
  B-pictures at all, since the decoder can't know in advance whether one
  is coming; total frame count and content are unaffected, only the
  timing of when the last one or two frames arrive within a `write()`
  call - Phase A/B's own test assets (no B-pictures) still pass
  unchanged under this scheme.
- As with Phase A/B's dequantization shifts, decodeInterBlock()'s shift
  (shared unchanged between P- and B-picture inter blocks) was
  re-confirmed - not just assumed to carry over - against a real
  bidirectionally-predicted B-picture macroblock's coefficients, not only
  the Phase B P-picture stream it was originally calibrated against - see
  `mpg_macroblock.h`'s file comment.
- **Bug found on real hardware, fixed**: calling `write()` again after
  `inputExhausted()` was already true (`setInput()`'s own documented use
  case - feeding more data in chunks, not just a one-shot decode) used to
  re-deliver the picture the *previous* call had already flushed, one
  extra time, before correctly resuming - `finalizePicture()`'s normal
  (boundary-triggered) anchor-release path didn't check
  `refFutureShown_` the way the end-of-input flush path already did, so
  it treated `refFuture_`'s already-shown content as freshly releasable.
  Found via `examples/DecodeFromProgmem`'s benchmark loop on a real
  ESP32 (30 repeated `write()` calls of a 10-picture clip delivered 119
  pictures, not the correct 300) - not by inspection. Fixed in
  `mpg_decoder.h`; regression-gated by
  `test/native/test_decode_resumed_input.cpp`.

## Encoder (`TinyMPGEncoder`, implemented) — intra-only, Phase 1

This session's library research found no realistic full-motion-estimation
MPEG-1 encoder that's portable to constrained microcontrollers (see
`architecture.md`'s Library survey — the only public-domain option with
true inter-frame compression, the Stanford PVRG/MSSG code, isn't a
realistic port target), so `TinyMPGEncoder` follows the `jo_mpeg`-style
path instead: every picture is a self-contained I-picture (no motion
estimation/compensation at all), the same trade-off this project's sibling
`arduino-audio-tools` already makes (MJPEG over inter-frame H.264) on its
most constrained targets.

- Forward DCT (`encoder/mpg_fdct.h`) - the orthogonal adjoint of the
  decoder's own `idct8x8()` (same shared cosine/normalization constants),
  operating on absolute sample values (no `-128` centering), matching how
  intra decode already works (see Phase A above).
- Forward quantization (`encoder/mpg_macroblock_encode.h`) does *not*
  algebraically invert the decoder's dequantization formula - it
  *simulates* it (tries nearby candidate levels through the exact same
  floor-shift-and-oddification dequant decodeIntraBlock() uses, clause
  7.4.4, and picks whichever reconstructs closest to the true
  coefficient). An earlier version that used a plain algebraic inverse
  measurably under-performed - caught by this phase's own correctness
  oracle (see below), the same way every decode-side dequant shift in
  this codebase was calibrated, not assumed from the formula alone.
  AC-coefficient magnitudes are also clamped to what MPEG-1's escape
  coding can actually represent (+/-255, `kMaxAcEscapeMagnitude`) *before*
  searching, not just at write time - letting the search see the wrong
  legal range once caused it to pick a magnitude that looked optimal in
  isolation but silently got clamped to something else entirely at write
  time, a real bug this phase's testing caught.
- No coded_block_pattern/DC-predictor-vs-reconstruction complexity: MPEG-1
  intra macroblocks have no spatial prediction (Phase A above), so this
  encoder needs no reconstructed-picture buffer at all - not deferred,
  structurally unnecessary. `TinyMPGEncoder`/`Encoder` (`encoder/
  mpg_encoder.h`) hold no `Frame<Allocator>` and take no `Allocator`
  template parameter as a result.
- Every `encodeFrame()` call writes a complete, independently-decodable
  unit (its own `sequence_header`/`group_of_pictures_header`/
  `picture_header`, not just resent on the first call) - see
  `encoder/mpg_headers_write.h`'s file comment for why, including the
  spec-defined "unspecified" placeholder values used for fields (like
  `bit_rate_value`/`vbv_delay`) this encoder has no real rate control to
  report an honest figure for.
- Verified against real `ffmpeg` decode, not just this project's own
  decoder: `ffprobe` correctly identifies the encoder's output as
  `mpeg1video` with the right width/height/pix_fmt, and `ffmpeg`'s own
  decode of it matches `TinyMPGDecoder`'s decode of the same bitstream
  exactly across a QP sweep (including near-lossless `qscale=1`) and a
  deliberately high-frequency/noisy synthetic stress pattern exercising
  the AC escape-coding path - see `docs/testing.md`.
- One decoder-side bug surfaced by this phase's own testing, fixed as
  part of it (not encoder-specific, but never exercised until this phase
  wrote the first single-picture stream this project's own decoder ever
  had to round-trip through `write()`): a stream whose *first* anchor
  picture is also its *last* previously could never be delivered at all -
  `Decoder::next()`'s end-of-input flush required a second `next()` call
  that a caller only looping while status stayed `kOk` would never make.
  Fixed in `decoder/mpg_decoder.h`.
- No custom quantizer matrices, no per-macroblock quantizer_scale
  adaptation, no rate control beyond a fixed `setQp()`, and (as with the
  decoder) no motion estimation/P- or B-picture encoding - all explicitly
  deferred future work, not silently assumed away.

## Out of scope indefinitely

- Interlaced content (MPEG-1 in practice is essentially always progressive;
  the small number of real-world interlaced MPEG-1 streams are not a
  target for this project).
- D-frames (DC-only pictures, a rarely-used MPEG-1 feature even in period
  encoders).
- Anything MPEG-2-specific (this is an MPEG-1 (ISO/IEC 11172-2) decoder,
  not MPEG-2 (ISO/IEC 13818-2) — no scalable/spatial extensions, no
  alternate scan, no 4:2:2/4:4:4 chroma).
