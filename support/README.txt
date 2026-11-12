The supplied 32x32 YUV420 MPEG-2 elementary stream has six pictures with I/P/B coding.
Prepared once using FFmpeg testsrc2 (rate=25, frames=6, mpeg2video, g=3, bf=1, q=5),
then a sequence-end code appended. FFmpeg is not needed to build or run this seed.
The build helper deliberately compiles only the portable library source subset, not the UI.
It includes any new libmpeg2/*_neon.c in target mode. Production integration in
configure.ac and libmpeg2/Makefile.am is part of the requested work; the helper is
not a replacement for that integration. No external conformance streams are bundled.

baseline.sh  builds the portable subset and decodes the bundled stream with the
             C back end forced (mpeg2_accel(0)); prints the picture count and a
             hash over every Y/Cb/Cr byte.

target.sh    builds the same subset plus tests/aarch64_contract.c, runs the idct,
             motion and stream assertion groups, and requires MPEG2_NEON_OK with
             a non-zero count in every group; then reruns baseline.sh.

bench.sh     compiles support/bench.c and the library sources with the same
             compiler and -O3, and reports scalar vs NEON times for the IDCT,
             for motion compensation, and for a whole-stream decode.  Each figure
             is a median over several runs after warm-up.  Pass a larger MPEG-2
             elementary stream as the first argument if the default fixture is
             too small to be dominated by the kernels.  It applies no pass/fail
             millisecond threshold: the numbers are machine dependent.

The moving-texture fixture used by the stream group is tests/neon_move.m2v; see
tests/README.txt for how it was produced and why it is needed.

