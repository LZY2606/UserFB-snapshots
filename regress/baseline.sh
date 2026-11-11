#!/bin/sh
set -eu
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT
cmake -S . -B "$b" -DPLUTOVG_BUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build "$b" -j2
cc -std=c11 -Iinclude regress/baseline.c "$b/libplutovg.a" -lm -o "$b/baseline"
"$b/baseline"
