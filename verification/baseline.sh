#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b/build" -DCMAKE_BUILD_TYPE=Release -DUNIT_TEST=OFF -DLIBYUV_DISABLE_JPEG=ON -DLIBYUV_DISABLE_ASM=ON
cmake --build "$b/build" --target yuv -j 4
c++ -std=c++17 -O1 -Iinclude verification/baseline.cc "$b/build/libyuv.a" -o "$b/witness"
"$b/witness"
