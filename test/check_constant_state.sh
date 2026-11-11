#!/bin/sh
# Builds and runs the resetToConstant() unit test with the system
# C++ compiler (C++17). Only the iir sources actually needed by the
# test are compiled; no external (numerical) libraries are required.
#
# Usage: sh test/check_constant_state.sh

set -e

cd "$(dirname "$0")/.."

CXX="${CXX:-c++}"
BUILD_DIR="$(mktemp -d "${TMPDIR:-/tmp}/constant_state.XXXXXX")"
trap 'rm -rf "$BUILD_DIR"' EXIT

"$CXX" -std=c++17 -Wall -Wextra -I. \
	test/constant_state.cpp \
	iir/Biquad.cpp \
	iir/RBJ.cpp \
	-o "$BUILD_DIR/constant_state"

"$BUILD_DIR/constant_state"
