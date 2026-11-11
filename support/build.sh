#!/usr/bin/env bash
set -euo pipefail
b=${1:?build directory required}
cmake -S . -B "$b" -DCMAKE_BUILD_TYPE=Release \
  -DGME_BUILD_SHARED=OFF -DGME_BUILD_STATIC=ON \
  -DGME_BUILD_EXAMPLES=OFF -DGME_BUILD_TESTING=OFF -DBUILD_TESTING=OFF \
  -DGME_ZLIB=OFF -DGME_YM2612_EMU=Nuked
cmake --build "$b" --target gme_static -j2
