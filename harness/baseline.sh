#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
cmake -S . -B "$build/build" -DWAVPACK_BUILD_PROGRAMS=OFF -DBUILD_TESTING=ON -DWAVPACK_ENABLE_ASM=OFF -DWAVPACK_ENABLE_DSD=OFF -DWAVPACK_ENABLE_LEGACY=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build "$build/build" -j 2
clang -std=c99 -Iinclude harness/baseline.c "$build/build/libwavpack.a" -lm -lpthread -o "$build/base"
"$build/base" "$build/six.wv"
"$build/build/wvtest" --seektest=1 "$build/six.wv" | tee "$build/seek.log"
grep -q 'pass' "$build/seek.log"
echo WAVPACK_OLD_SEEK_OK
if [[ ${1:-} == target ]]; then
  test -s harness/channel_projection.c
  clang -std=c99 -Iinclude harness/channel_projection.c "$build/build/libwavpack.a" -lm -lpthread -o "$build/projection"
  "$build/projection" "$build" | tee "$build/target.log"
  grep -qx 'WAVPACK_CHANNEL_PROJECTION_OK cases=8' "$build/target.log"
fi
