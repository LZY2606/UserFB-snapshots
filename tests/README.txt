tests/neon_move.m2v - fixture for the AArch64 NEON contract test
=================================================================

What it is
----------
A small MPEG-2 elementary stream used by tests/aarch64_contract.c.  It
exists so the stream group exercises, and can prove it exercises, the
parts of the decoder a static I/P/B fixture does not reliably reach:

  * bidirectional (B) prediction, so the "average two already-rounded
    predictions" path runs;
  * strong sub-pixel motion, so the horizontal, vertical and diagonal
    half-pel put/avg kernels (o/x/y/xy) all run at both 8 and 16 pixels
    wide;
  * repeated content between pictures, so P and B macroblocks actually
    carry motion vectors and residual blocks.

Provenance
----------
Generated once, at task time, with FFmpeg 8.1.2 from the lavfi
"testsrc2" source, then frozen into the repository:

    ffmpeg -f lavfi \
        -i "testsrc2=size=96x96:rate=25:duration=0.4" \
        -vf "rotate=0.35*sin(2*PI*t/0.5):fillcolor=black,scale=96x96" \
        -c:v mpeg2video -g 6 -bf 2 -q:v 12 -pix_fmt yuv420p \
        -f mpeg2video tests/neon_move.m2v

A sequence-end code (00 00 01 b7) was appended so the decoder terminates
cleanly.  The stream is deterministic: decoding it with the portable C
back end always produces the same ten pictures in the same order.

The file is checked in.  Nothing regenerates it at test time and FFmpeg
is not needed to build or run the tests.

Contents
--------
96x96, YUV 4:2:0, ten displayed pictures, I/P/B, B pictures present.
Decoding with the C back end produces the reference hash recorded in
tests/aarch64_contract.c (GOLDEN_MOVE_HASH); the NEON back end must
produce the identical hash and the identical picture types and
temporal_reference order.

Licence
-------
The pixel content comes from FFmpeg's synthetic "testsrc2" generator; no
third-party footage is included.
