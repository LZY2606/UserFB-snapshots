#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
work=$(mktemp -d "${TMPDIR:-/tmp}/pgl-old.XXXXXX")
trap 'rm -rf "$work"' EXIT
bash task_support/amalgamate.sh > "$work/portablegl.h"
c++ -std=c++20 -O2 -DUSING_PORTABLEGL -I"$work" -Iglcommon -Iexternal testing/run_tests.cpp glcommon/gltools.cpp -lm -o "$work/old"
cd testing
"$work/old" hello_indexing0 clip_xy_fill zbuf_depthon zbuf_depthon_fliprange zbuf_depthon_maskoff fbo_depth fbo_blit_depth fbo_clear_buffer
