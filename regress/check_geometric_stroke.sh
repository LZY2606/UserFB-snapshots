#!/bin/sh
set -eu

# Geometric stroke regression: numeric assertions plus a small PNG.
# All build intermediates live in a temporary directory; the rendered PNG is
# written next to this script so it can be inspected directly.
b=$(mktemp -d)
trap 'rm -rf "$b"' EXIT

png="$(dirname "$0")/geometric_stroke.png"

cmake -S . -B "$b" -DPLUTOVG_BUILD_EXAMPLES=OFF -DBUILD_SHARED_LIBS=OFF
cmake --build "$b" -j2
cc -std=c11 -Iinclude regress/geometric_stroke.c "$b/libplutovg.a" -lm -o "$b/geometric_stroke"
"$b/geometric_stroke" "$png"
echo "PNG: $png"
