#!/usr/bin/env bash
set -euo pipefail

# Build in the per-session temp dir (mktemp's default target is not writable
# in every sandboxed environment).
build="${TMPDIR:-/tmp}/phase_demo_build_$$"
mkdir -p "$build"
trap 'rm -rf "$build"' EXIT

extra=()
if [[ $(uname -s) == Darwin ]]; then
  # Accelerate is used by the DSP routines; the full library also pulls in
  # AudioFileUtils, which links against AudioToolbox/CoreFoundation on macOS.
  extra=(-framework Accelerate -framework AudioToolbox -framework CoreFoundation)
fi

c++ -std=c++11 -Isrc -Isrc/Tonic sanity/phase_demo.cpp src/Tonic/*.cpp "${extra[@]}" -o "$build/phase_demo"
"$build/phase_demo"
