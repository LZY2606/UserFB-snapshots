#!/bin/sh
# Build and run the bilinear negative-size (mirroring) regression checks.
# Everything is rendered into in-memory pixel buffers: no windows and no image
# libraries are involved.
set -eu

# Resolve the repository root from the script location so the test can be
# launched from any working directory.
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$(dirname -- "$script_dir")"

build_dir=${TMPDIR:-/tmp}/olivec-bilinear-flip
mkdir -p "$build_dir"

cc -std=c99 -Wall -Wextra -I. regress/baseline_bilinear.c -lm \
    -o "$build_dir/baseline_bilinear"
cc -std=c99 -Wall -Wextra -I. regress/check_bilinear_flip.c -lm \
    -o "$build_dir/check_bilinear_flip"

# baseline_bilinear: the 2x2 -> 4x4 red matrix from the task plus a mirror
# check on the same texture (sentinels guard the stride padding).
"$build_dir/baseline_bilinear"
# check_bilinear_flip: four flip directions, four-edge clipping compared
# against a full render cropped afterwards, sentinel-padded source/destination
# strides (incl. olivec_subcanvas views), single-pixel axes, empty sources and
# positive-size compatibility.
"$build_dir/check_bilinear_flip"

echo "check_bilinear_flip: all tests passed"
