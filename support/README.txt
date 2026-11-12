The supplied 32x32 YUV420 MPEG-2 elementary stream has six pictures with I/P/B coding.
Prepared once using FFmpeg testsrc2 (rate=25, frames=6, mpeg2video, g=3, bf=1, q=5),
then a sequence-end code appended. FFmpeg is not needed to build or run this seed.
The build helper deliberately compiles only the portable library source subset, not the UI.
It includes any new libmpeg2/*_neon.c in target mode. Production integration in
configure.ac and libmpeg2/Makefile.am is part of the requested work; the helper is
not a replacement for that integration. No external conformance streams are bundled.
