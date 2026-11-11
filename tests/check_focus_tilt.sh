#!/bin/sh
#
# Headless C regression for the tilted thin-lens focus plane.
# Configures a fresh Release build in a temporary directory, links the
# regression against the built objects, and runs it without any window.
#
set -eu

# Run from the repository root regardless of the caller's CWD.
cd "$(dirname "$0")/.."

build=$(mktemp -d "${TMPDIR:-/tmp}/cray-focus-tilt.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

echo "Configuring Release build in $build"
cmake -S . -B "$build" -DCMAKE_BUILD_TYPE=Release -DNO_IPO=ON > "$build/configure.log"
echo "Building c-ray objects"
cmake --build "$build" --target c-ray -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)" > "$build/build.log"

# Reuse every lib/common object produced by the real build so the test
# exercises the same camera, sampler, API and serializer code as production.
objects=$(find "$build/CMakeFiles/lib.dir" "$build/CMakeFiles/common.dir" -name '*.o')

echo "Compiling focus tilt regression"
cc -std=c99 -O2 -D_POSIX_C_SOURCE=200112L -Wall -Wextra \
	-Iinclude -Isrc -Isrc/lib -Isrc/common \
	tests/focus_tilt_regression.c $objects \
	-lpthread -lm -ldl -o "$build/focus_tilt"

echo "Running focus tilt regression"
"$build/focus_tilt"
