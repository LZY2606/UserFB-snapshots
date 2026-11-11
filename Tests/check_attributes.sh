#!/bin/sh
#
# Builds and runs the per-vertex attribute interpolation tests.
# Usage: sh tests/check_attributes.sh
#
# Compiles a C driver together with the libtess2 sources, checks intersection
# interpolation, repeated edge splitting, crossing contours, all three output
# layouts and tesselator reuse, exercises allocation failures through a custom
# allocator, and writes a grayscale PPM rendered from the interpolated
# attributes. Exits non-zero on any failure.

set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

BUILD_DIR="${TMPDIR:-/tmp}/check_attributes.$$"
mkdir -p "$BUILD_DIR"

SOURCES="
	$ROOT/Source/bucketalloc.c
	$ROOT/Source/dict.c
	$ROOT/Source/geom.c
	$ROOT/Source/mesh.c
	$ROOT/Source/priorityq.c
	$ROOT/Source/sweep.c
	$ROOT/Source/tess.c
"

BIN="$BUILD_DIR/check_attributes"
PPM="$BUILD_DIR/check_attributes.ppm"

cc -std=c99 -Wall -Wextra -Wpedantic -O2 \
	-I"$ROOT/Include" -I"$ROOT/Source" \
	$SOURCES "$SCRIPT_DIR/check_attributes.c" \
	-lm -o "$BIN"

"$BIN" "$PPM"

echo "PPM output: $PPM"
