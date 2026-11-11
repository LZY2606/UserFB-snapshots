#!/usr/bin/env bash
set -euo pipefail
b=$(mktemp -d "${TMPDIR:-/tmp}/xvc-baseline.XXXXXX")
trap 'find "$b" -depth -delete' EXIT
cmake -S . -B "$b/build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DXVC_TARGET_ARCH=ARM -DENABLE_ASSEMBLY=OFF -DBUILD_TESTS=OFF -DBUILD_APPS=OFF -DHIGH_BITDEPTH=ON
cmake --build "$b/build" --target xvc_enc_lib xvc_dec_lib -j4
c++ -std=c++11 -DXVC_HIGH_BITDEPTH=1 -Isrc tests/input_baseline.cpp "$b/build/src/libxvcenc.a" "$b/build/src/libxvcdec.a" -pthread -o "$b/witness"
"$b/witness"
