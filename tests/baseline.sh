#!/usr/bin/env bash
set -euo pipefail
root=$PWD
b=$(mktemp -d)
trap 'find "$b" -depth -delete' EXIT
# Upstream version generation writes into SRC_PATH, so build a temporary copy.
tar -cf - Makefile build codec | tar -xf - -C "$b"
make -C "$b" -j4 USE_ASM=No HAVE_GMP_API=No HAVE_GTEST=No libopenh264.a
c++ -std=c++17 -Icodec/api/wels tests/codec_baseline.cpp "$b/libopenh264.a" -pthread -o "$b/witness"
"$b/witness"
