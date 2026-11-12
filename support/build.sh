#!/bin/bash
set -euo pipefail
R=$(pwd)
B=${CHECK_BUILD_DIR:-$(mktemp -d)}
if [[ -z ${CHECK_BUILD_DIR:-} ]]; then trap 'rm -rf "$B"' EXIT; fi
mkdir -p "$B"
cmake -S "$R" -B "$B" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_SDL=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DENABLE_SHERLOCK265=OFF -DENABLE_SIMD=OFF -DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF -DWITH_FUZZERS=OFF
cmake --build "$B" --target de265 -j4
compile() { c++ -std=c++17 -I"$B" -I"$B/libde265" -I"$R" -I"$R/support" "$1" "$B/libde265/libde265.a" -lpthread -o "$2"; }
compile_target() { c++ -std=c++17 -I"$B" -I"$B/libde265" -I"$R" -I"$R/support" "$1" "$R/support/driver.c" "$B/libde265/libde265.a" -lpthread -o "$2"; }
