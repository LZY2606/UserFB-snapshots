# Preparing input with ffmpeg

This decoder only implements a deliberately small subset of MPEG-1 (see
[Scope](scope.md) — Phase A/B/C: I-, P-, and B-frames; D-frames are the
only picture type still unsupported); source video needs to be transcoded
to match it, the same way TinyH264's own
[equivalent doc](https://github.com/pschatzmann/TinyH264/blob/main/docs/preparing-input-with-ffmpeg.md)
describes for H.264/Baseline.

## Phase A (I-frame only) test/validation streams

```sh
ffmpeg -i input.mp4 \
  -vf scale=176:144 -pix_fmt yuv420p \
  -c:v mpeg1video -g 0 -bf 0 \
  -f mpeg1video output.m1v
```

What each part is doing, and why it's required (not just a good default):

- **`-f mpeg1video` (and a plain `output.m1v`/`.mpg` filename)** - forces
  ffmpeg's raw MPEG-1 video elementary-stream muxer: `picture_start_code`
  (`00 00 01 00`)-delimited access units with no Program Stream (pack/PES)
  framing around them. This is the only framing `TinyMPGDecoder` understands
  directly - if you need the Program Stream framing too (e.g. to produce or
  consume an actual `.mpg`/`.vob`-style file), see
  [arduino-audio-tools](https://github.com/pschatzmann/arduino-audio-tools)'
  `MuxerMPG`/`DemuxerMPG` (`ContainerMPG.h`), which strips/adds exactly that
  layer around the same elementary-stream bytes.
- **`-g 0`** - forces every frame to be encoded as an I-frame (GOP size 0 =
  no P-frames referencing a previous picture) - use this recipe (rather
  than the Phase B one below) when you specifically want an I-only stream,
  e.g. to isolate whether a decode issue is intra- or inter-related.
- **`-bf 0`** - disables B-frames, for when you specifically want a
  B-frame-free stream (e.g. to isolate whether a decode issue is B-picture-
  specific, or to keep output-order == decode-order for easier debugging -
  see `mpg_decoder.h`'s file comment on display-order reordering). Phase C
  now decodes B-frames, so this is no longer required for correctness,
  just useful for isolating issues to the Phase A/B code paths.
- **`-pix_fmt yuv420p`** - MPEG-1 is always 4:2:0 planar; ffmpeg needs this
  explicit if the source isn't already in that format.

## Phase B (I- and P-frames) test/validation streams

```sh
ffmpeg -i input.mp4 \
  -vf scale=176:144 -pix_fmt yuv420p \
  -c:v mpeg1video -g 10 -bf 0 \
  -f mpeg1video output.m1v
```

Same as the Phase A recipe, except **`-g 10`** sets a real GOP size (one
I-picture followed by 9 P-pictures, repeating) instead of forcing every
picture to be intra - any positive GOP size works, since
`TinyMPGDecoder` doesn't care how many P-pictures follow an I-picture,
only that a P-picture is always preceded (eventually) by one. `-bf 0`
keeps this recipe P-frame-only, isolating it from Phase C's B-picture path.

## Phase C (I-, P-, and B-frames) test/validation streams

```sh
ffmpeg -f lavfi -i "testsrc=size=176x144:rate=25:duration=0.4" \
  -vf format=yuv420p \
  -c:v mpeg1video -g 9 -bf 2 -qscale:v 3 \
  -f mpeg1video output.m1v
```

Differences from the Phase B recipe:

- **`-bf 2`** - enables B-frames, up to 2 consecutive B-pictures between
  each pair of anchor (I/P) pictures - this is what actually exercises
  Phase C's backward/bidirectional motion compensation and the
  display-order output reordering; without it, ffmpeg's `mpeg1video`
  encoder defaults to zero B-frames and this recipe would be no different
  from Phase B's.
- **`-g 9`** together with `-bf 2` produces a repeating I,P,B,B GOP
  pattern (decode order) - `TinyMPGDecoder` delivers frames in *display*
  order instead (I,B,B,P,B,B,P,...), so don't assume the Nth frame handed
  to your callback corresponds to the Nth picture in the raw bitstream -
  see `mpg_decoder.h`'s file comment and `test_decode_bframe.cpp`.
- This example synthesizes its own input via `-f lavfi testsrc=...`
  instead of transcoding a source file, purely so this recipe is
  copy-pasteable without needing a source video on hand - swap in
  `-i input.mp4 -vf scale=176:144,format=yuv420p` the same way the Phase
  A/B recipes do if you're starting from real footage.

## Generating the oracle decode (for the native test suite)

```sh
ffmpeg -i output.m1v -pix_fmt yuv420p output_ref.yuv
```

This raw planar YUV420 file is what `test/native/test_decode_iframe.cpp`/
`test_decode_pframe.cpp`/`test_decode_bframe.cpp` (see [Testing](testing.md))
diff `TinyMPGDecoder`'s own decode output against - `ffmpeg`'s own decoder
is this project's correctness oracle, the same role it plays for
TinyH264's test suite. `ffmpeg -i output.m1v -pix_fmt yuv420p` already
decodes (and outputs) pictures in display order, matching
`TinyMPGDecoder`'s own delivery order, so no extra reordering is needed to
compare the two. The comparison is tolerance-based, not byte-exact - see
`test_decode_iframe.cpp`'s file comment for why.
