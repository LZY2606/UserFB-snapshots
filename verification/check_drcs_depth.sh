#!/usr/bin/env bash
#
# Build and run the DRCS grayscale-depth tests in a temporary directory.
#
# The test preserves the input from verification/baseline.cc (3x1, depth 3,
# samples 0,1,2 packed as 0x18) and asserts the corrected pixels, alongside
# decode/render checks for depths 2..256.
#
set -eu

b=$(mktemp -d "${TMPDIR:-/tmp}/aribcc_drcs_depth.XXXXXX")
trap 'rm -rf "$b"' EXIT

cmake -S . -B "$b/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DARIBCC_BUILD_TESTS=ON \
    -DARIBCC_USE_EMBEDDED_FREETYPE=OFF
cmake --build "$b/build" --target test_drcs_depth -j 4

"$b/build/test_drcs_depth"
