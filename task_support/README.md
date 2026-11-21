The supplied fixture is nine 64x48 YUV420 MPEG-2 pictures with moving,
plane-distinct texture (I/P/B). It is ordinary baseline material, not field
coverage. Bytes are in fixture.h; a sequence_end_code follows the encoded data.
The texture was generated locally and is dedicated to CC0. The library
continues to use its upstream GPL-2.0-or-later license.

bash task_support/run.sh baseline
bash task_support/run.sh target

The target file tests/plane_layout_cases.c is intentionally absent. Register
layout, prediction, fields and compat groups through check.h. The provided
driver requires nonzero assertions and a normal return for every case and group.
The build script compiles libmpeg2/*.c and libmpeg2/convert/*.c into the real
library; new production files in those directories participate in the build.
It selects the portable C implementation. No package download or host media
encoder is used at runtime. CC can select a system C compiler.

The texture was encoded once in advance with FFmpeg mpeg2video using
-q:v 3 -g 9 -bf 2. FFmpeg is not needed to run either command. Add field-specific
fixtures and any extra C helpers beside the new regression file; keep the
single entry file linkable with the supplied driver.
