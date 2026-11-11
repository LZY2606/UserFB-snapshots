#!/bin/sh
# Build and run the standalone v210 tests in a temporary directory.  The test
# has its own main() and must not require GoogleTest at build or run time.
set -eu

repo=$(cd "$(dirname "$0")/.." && pwd)
b=$(mktemp -d "${TMPDIR:-/tmp}/check_v210.XXXXXX")
trap 'rm -rf "$b"' EXIT

cmake -S "$repo" -B "$b/build" -DCMAKE_BUILD_TYPE=Release -DUNIT_TEST=OFF \
      -DLIBYUV_DISABLE_JPEG=ON -DLIBYUV_DISABLE_ASM=ON
cmake --build "$b/build" --target yuv -j 4

c++ -std=c++17 -O1 -I"$repo/include" "$repo/unit_test/v210_test.cc" \
    "$b/build/libyuv.a" -o "$b/v210_test"
"$b/v210_test"
