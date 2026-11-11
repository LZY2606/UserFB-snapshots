#!/usr/bin/env bash
# Builds the planar intake regression in temporary build directories and
# runs it twice:
#   HIGH_BITDEPTH=ON  -> full matrix (8/8, 8/10, 10/10, 10/12 input/internal)
#   HIGH_BITDEPTH=OFF -> 8 bit input/internal matrix only
# Every run must report non-zero case counts and print the success marker.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/xvc-planar-intake.XXXXXX")
trap 'find "$work" -depth -delete' EXIT

run_config() {
  local high_bitdepth="$1"
  local compile_flag
  if [ "$high_bitdepth" = "ON" ]; then
    compile_flag=-DXVC_HIGH_BITDEPTH=1
  else
    compile_flag=-DXVC_HIGH_BITDEPTH=0
  fi
  local build_dir="$work/build-$high_bitdepth"
  echo "== configuring HIGH_BITDEPTH=$high_bitdepth =="
  cmake -S "$root" -B "$build_dir" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DXVC_TARGET_ARCH=ARM -DENABLE_ASSEMBLY=OFF \
    -DBUILD_TESTS=OFF -DBUILD_APPS=OFF \
    -DHIGH_BITDEPTH="$high_bitdepth" >/dev/null
  cmake --build "$build_dir" --target xvc_enc_lib xvc_dec_lib -j4
  c++ -std=c++11 "$compile_flag" -I"$root/src" -I"$root/tests" \
    "$root/tests/planar_intake.cpp" "$root/tests/planar_intake_ref.cc" \
    "$build_dir/src/libxvcenc.a" "$build_dir/src/libxvcdec.a" \
    -pthread -o "$work/planar_intake-$high_bitdepth"
  PLANAR_INTAKE_FAIL_FILE="$work/fail-$high_bitdepth.txt" \
    "$work/planar_intake-$high_bitdepth" | tee "$work/result-$high_bitdepth.txt"
  grep -Eq 'resampler check cases: [1-9][0-9]*' \
    "$work/result-$high_bitdepth.txt"
  grep -Eq 'end-to-end cases: [1-9][0-9]*' \
    "$work/result-$high_bitdepth.txt"
  grep -q 'xvc planar intake: ok' "$work/result-$high_bitdepth.txt"
}

run_config ON
run_config OFF
echo "xvc planar intake: ok"
