# Architecture

## Why this project exists

[arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools)
already has `ContainerMPG.h` (`MuxerMPG`/`DemuxerMPG`), which muxes/demuxes
the ISO/IEC 11172-1 Program Stream — but it treats the MPEG-1 video
elementary stream as an opaque, already-encoded blob; it never touches
pixels. To actually play (or produce) MPEG-1 video, a real *codec* is
needed: something that implements arduino-audio-tools' `VideoDecoder`/
`VideoEncoder` interfaces (`AudioTools/Video/CodecVideo.h`), the role
[TinyH264](https://github.com/pschatzmann/TinyH264) fills for H.264 (wrapped
by that project's `CodecH264.h`).

No embeddable MPEG-1 codec library exists that was worth wrapping instead of
writing this from scratch (see the Library survey below), so TinyMPG follows
TinyH264's own structure, conventions, and quality bar: header-only C++, no
dynamic allocation in the hot path, PSRAM-friendly, and validated
pixel-exact against real `ffmpeg`-encoded streams rather than hand-crafted
test bitstreams or conformance-suite guesswork.

arduino-audio-tools will eventually get a thin `CodecMPG.h` wrapper around
TinyMPG, mirroring `CodecH264.h` — that wrapper lives in that project, not
here.

## Library survey (why "from scratch")

- **`pl_mpeg`** (MIT, single header, ~3000 lines, no dependencies) is a
  solid, fast, portable MPEG-1 video + MP2 audio *decoder* — but it's still
  decode-only, and using it would mean writing a `VideoDecoder` wrapper from
  scratch around it anyway. Not chosen because building the decoder
  directly gives full control over memory layout (PSRAM placement,
  incremental picture buffers) the way TinyH264 does for H.264, rather than
  adapting to `pl_mpeg`'s own internal buffering assumptions.
- **`jo_mpeg`** (public domain, single header): a genuinely
  embedded-feasible MPEG-1 *encoder*, but intra-only (every frame coded as
  an I-frame, no motion estimation/compensation at all) — a real trade-off,
  not a full ISO/IEC 11172-2 encoder. Relevant to TinyMPG's future encoder
  phase, not the decoder.
- **Stanford PVRG/MSSG reference codec** (public domain, both encode and
  decode, true inter-frame motion compensation): the only public-domain
  option with full spec fidelity, but it's 1990s workstation C, explicitly
  "kludgey" by its own documentation, and not realistically portable to
  ESP32-class hardware without a near-total rewrite.
- **`libmpeg2`** (GPL): a real, fast, portable MPEG-1/2 decoder library —
  but a full autoconf/make library, not header-only, so it doesn't fit this
  project's "just add a Arduino library and #include a header" distribution
  model the way `pl_mpeg`/TinyH264 do.

## Directory layout

```
TinyMPG/
  library.properties        # Arduino Library Manager metadata
  LICENSE                   # GPLv3
  README.md
  idf_component.yml         # ESP-IDF component manifest
  CMakeLists.txt            # INTERFACE library target `TinyMPG`; test/native/ subdir
  docs/
    architecture.md         # this file
    scope.md                # what's implemented/validated vs. deferred
    memory-budget.md        # static/heap RAM cost (TBD until measured on real hardware)
    decoding.md             # TinyMPGDecoder usage
    encoding.md              # TinyMPGEncoder usage
    testing.md               # native CMake/CTest suite, how test assets are (re)generated
    preparing-input-with-ffmpeg.md  # the exact ffmpeg recipe this decoder needs
  src/
    mpg_config.h             # compile-time limits: MPG_MAX_WIDTH/HEIGHT, MPG_MAX_ES_BUFFER
    StdAllocator.h           # default allocator - std::allocator-shaped, but never throws (returns nullptr on failure) - see memory-budget.md
    PSRAMAllocatorESP32.h    # std::allocator-shaped PSRAM allocator (ported from TinyH264, not H.264-specific); also never throws, same contract as StdAllocator.h
    common/
      mpg_frame.h            # Frame<Allocator>: YUV 4:2:0 planar picture buffer
      mpg_tables.h            # zigzag scan order, default intra/non-intra quantizer matrices, frame_rate_code/aspect_ratio tables
      mpg_vlc_tables.h        # DC size, DCT-coefficient run/level, macroblock_address_increment, macroblock_type (P and B), coded_block_pattern, and motion_code VLC tables (ISO/IEC 11172-2 Annex B)
    decoder/
      mpg_bitreader.h          # MSB-first bit reader: fixed-length u(n) reads + generic VLC table walk (no Exp-Golomb, unlike H.264)
      mpg_headers.h             # sequence_header / picture_header parsing
      mpg_idct.h                 # 8x8 inverse DCT + Clip1
      mpg_motion.h                # motion_code/motion_residual decode + half-pel luma/chroma motion compensation, forward-only (Phase B) and bidirectional (Phase C)
      mpg_macroblock.h            # macroblock_address_increment/macroblock_type/coded_block_pattern/block-layer (I-, P-, and B-picture) parsing & reconstruction
      mpg_decoder.h                # top-level Decoder<Allocator>: start-code demux, picture-boundary detection, two-reference management, display-order output reordering, setInput()/next()/DecodeStatus
      mpg_rgb.h                    # YUV420 -> RGB565/RGB666/RGB888 conversion
    encoder/
      mpg_bitwriter.h              # MSB-first bit writer + VLC/byte-alignment/start-code helpers - write-side mirror of decoder/mpg_bitreader.h
      mpg_fdct.h                    # 8x8 forward DCT - orthogonal adjoint of decoder/mpg_idct.h's idct8x8()
      mpg_macroblock_encode.h       # forward quantization (DC/AC) + differential-DC/run-level-AC VLC write - intra-only, write-side mirror of decoder/mpg_macroblock.h
      mpg_headers_write.h           # sequence_header/group_of_pictures_header/picture_header writing
      mpg_encoder.h                 # top-level Encoder: no Allocator/Frame - intra-only needs no picture buffer, see docs/scope.md
    TinyMPGDecoder.h            # public-facing push-data/get-callback API
    TinyMPGEncoder.h            # public-facing encodeFrame() API
  examples/
    DecodeFromProgmem/          # decode a tiny embedded I-frame-only clip, print per-frame stats - no extra hardware needed
    DecodeToDisplay/            # decode + push to an ILI9341 SPI TFT via TinyGPU, band by band
    EncodeSyntheticFrame/       # encode a synthetic gradient, print per-frame stats - no extra hardware needed
  test/
    native/
      CMakeLists.txt            # explicit list of test executables (not a glob), same pattern as TinyH264
      assets/                   # ffmpeg-generated *.m1v + ffmpeg-decoded *.yuv oracle files
      test_*.cpp
```

The `coded_block_pattern`/`motion_code`/`motion_residual` machinery above
is shared between Phase B (P-frames) and Phase C (B-frames, forward/
backward/bidirectional motion compensation, two-reference management and
display-order output reordering) - see [Scope](scope.md) for what each
phase adds. `src/encoder/` holds the intra-only encoder (Phase 1, see
[Scope](scope.md)'s Encoder section) - a proper sibling directory to
`src/decoder/`, not (yet) a from-scratch full encoder implementing motion
estimation/P- or B-picture encoding.

## Integration contract with `DemuxerMPG` (arduino-audio-tools)

`DemuxerMPG::setOutputVideo()` forwards raw ISO/IEC 11172-2 bytes,
self-delimited by `picture_start_code` (`00 00 01 00`) sequences — the same
kind of self-delimiting start code `H264Decoder` relies on for Annex-B NAL
units. It additionally calls `flush()` at real access-unit boundaries (when
a new PES packet carries a PTS) and at stream end. `TinyMPGDecoder::write()`
detects picture boundaries itself from `00 00 01 00` (matching
`H264Decoder`'s NAL-start-code-driven approach rather than relying on the
caller's `flush()` timing), so it decodes correctly whether or not the
upstream `flush()` calls line up exactly with picture boundaries —
`flush()` itself stays a no-op on the decoder side, same as `H264Decoder`'s.

`DemuxerMPG` also does its own light-weight `sequence_header` probing
(`probeVideoHeader()` in `ContainerMPG.h`) purely to report `width`/
`height`/`fps` via `getVideoInfo()` before any real decoder runs — this is
independent of, and duplicated by, TinyMPG's own (necessarily more
complete) `sequence_header` parsing in `mpg_headers.h`.
