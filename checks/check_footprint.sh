#!/bin/sh
# Windowless footprint regression for swr::Texture.
#
# Builds and runs in a temporary directory (no window / SDL video needed),
# exercises the rotation-invariant anisotropic sampler and writes a small
# comparison PPM into the current working directory. Exits non-zero on any
# failed assertion.

set -u

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

WORK=$(mktemp -d "${TMPDIR:-/tmp}/footprint-check.XXXXXX") || exit 1
trap 'rm -rf "$WORK"' EXIT

if command -v sdl2-config >/dev/null 2>&1; then
    SDL_CFLAGS=$(sdl2-config --cflags)
    SDL_LIBS=$(sdl2-config --libs)
else
    SDL_CFLAGS="-I/opt/homebrew/include/SDL2 -D_THREAD_SAFE"
    SDL_LIBS="-L/opt/homebrew/lib -lSDL2"
fi

CXX=${CXX:-c++}
# The renderer headers carry MSVC-only pragmas and a pre-existing unused
# variable; keep the check output focused on real diagnostics.
CXXFLAGS="-std=c++17 -Wall -Wextra -Wno-unknown-pragmas -Wno-unused-variable -O1 -I$ROOT $SDL_CFLAGS"

status=0

echo "== building baseline reproduction =="
if ! $CXX $CXXFLAGS "$ROOT/checks/baseline.cpp" $SDL_LIBS -o "$WORK/baseline"; then
    echo "baseline build failed" >&2
    exit 1
fi
if ! "$WORK/baseline"; then
    status=1
fi

echo "== building footprint regression =="
if ! $CXX $CXXFLAGS "$ROOT/checks/footprint_test.cpp" $SDL_LIBS -o "$WORK/footprint_test"; then
    echo "footprint_test build failed" >&2
    exit 1
fi

PPM="$PWD/footprint_compare.ppm"
echo "== running footprint regression (PPM: $PPM) =="
if ! "$WORK/footprint_test" "$PPM"; then
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "check_footprint: OK"
else
    echo "check_footprint: FAILURES DETECTED" >&2
fi
exit $status
