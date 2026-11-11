#!/usr/bin/env bash
# Mutation self check for the planar intake regression.
#
# Two independent temporary copies of the source tree are created, each with
# exactly one deliberately injected defect in src/xvc_common_lib/resample.cc:
#
#   stride   : the high bit depth strided intake branch advances rows by the
#              compact sample width instead of the caller supplied stride
#   upshift  : 8 bit input samples are no longer upshifted to the internal
#              bit depth
#
# Both copies must still compile. Each mutated library must then be rejected
# by a sample or NAL assertion that reports the failing test case, plane and
# coordinates. A crash, a compile failure or a clean pass is treated as a
# self check failure. The original working tree is never modified.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/xvc-planar-mutation.XXXXXX")
keep_work_on_failure() {
  trap - EXIT
  echo "self check failed, keeping work directory: $work" >&2
}
trap 'find "$work" -depth -delete' EXIT

status_before=$(git -C "$root" status --porcelain | sort)

apply_stride_mutation() {
  python3 - "$1" <<'PY'
import sys
path = sys.argv[1] + "/src/xvc_common_lib/resample.cc"
with open(path) as handle:
    content = handle.read()
anchor = """      const uint16_t *pic16 =
        reinterpret_cast<const uint16_t *>(&input_bytes[0]);
      for (int x = 0; x < width; x++) {
        dst[x] = static_cast<Sample>(pic16[x] << bit_shift);
      }
      input_bytes += input_stride;"""
replacement = anchor.replace(
    "input_bytes += input_stride;",
    "input_bytes += static_cast<ptrdiff_t>(width * 2);")
if content.count(anchor) != 1:
    sys.exit("stride mutation anchor not found exactly once")
with open(path, "w") as handle:
    handle.write(content.replace(anchor, replacement))
PY
}

apply_upshift_mutation() {
  python3 - "$1" <<'PY'
import sys
path = sys.argv[1] + "/src/xvc_common_lib/resample.cc"
with open(path) as handle:
    content = handle.read()
anchor = "dst[x] = static_cast<Sample>(input_bytes[x] << bit_shift);"
replacement = "dst[x] = static_cast<Sample>(input_bytes[x]);"
if content.count(anchor) != 1:
    sys.exit("upshift mutation anchor not found exactly once")
with open(path, "w") as handle:
    handle.write(content.replace(anchor, replacement))
PY
}

run_mutation() {
  local name="$1"
  local mutator="$2"
  local copy="$work/copy-$name"
  mkdir -p "$copy"
  # Copy only the build inputs; .git and other working tree state stay out.
  cp -R "$root/src" "$root/tests" "$root/cmake" "$root/CMakeLists.txt" \
    "$copy/"
  "$mutator" "$copy"

  echo "== mutation $name: building mutated library =="
  cmake -S "$copy" -B "$copy/build" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DXVC_TARGET_ARCH=ARM -DENABLE_ASSEMBLY=OFF \
    -DBUILD_TESTS=OFF -DBUILD_APPS=OFF \
    -DHIGH_BITDEPTH=ON >/dev/null
  if ! cmake --build "$copy/build" --target xvc_enc_lib xvc_dec_lib -j4 \
      >"$work/build-$name.log" 2>&1; then
    echo "mutation $name: mutated library failed to compile" >&2
    keep_work_on_failure
    exit 1
  fi
  if ! c++ -std=c++11 -DXVC_HIGH_BITDEPTH=1 -I"$copy/src" -I"$copy/tests" \
      "$copy/tests/planar_intake.cpp" \
      "$copy/tests/planar_intake_ref.cc" \
      "$copy/build/src/libxvcenc.a" "$copy/build/src/libxvcdec.a" \
      -pthread -o "$work/mutation-$name" 2>"$work/compile-$name.log"; then
    echo "mutation $name: regression failed to compile" >&2
    keep_work_on_failure
    exit 1
  fi

  set +e
  PLANAR_INTAKE_FAIL_FILE="$work/fail-$name.txt" \
    "$work/mutation-$name" >"$work/run-$name.log" 2>&1
  local exit_code=$?
  set -e
  if [ "$exit_code" -eq 0 ]; then
    echo "mutation $name: NOT detected (regression passed unexpectedly)" >&2
    keep_work_on_failure
    exit 1
  fi
  if [ "$exit_code" -ge 128 ]; then
    echo "mutation $name: process crashed (signal), not a valid detection" >&2
    keep_work_on_failure
    exit 1
  fi
  if ! grep -Eq 'MISMATCH case=.* plane=[0-9]+ x=-?[0-9]+ y=-?[0-9]+' \
      "$work/run-$name.log"; then
    echo "mutation $name: failure did not report case, plane and coordinates" >&2
    keep_work_on_failure
    exit 1
  fi
  echo "mutation $name: detected (exit=$exit_code)"
  grep -m1 'MISMATCH' "$work/run-$name.log"
}

run_mutation stride apply_stride_mutation
run_mutation upshift apply_upshift_mutation

status_after=$(git -C "$root" status --porcelain | sort)
if [ "$status_before" != "$status_after" ]; then
  echo "working tree changed while running the mutation self check" >&2
  keep_work_on_failure
  exit 1
fi
echo "xvc planar intake mutations: ok"
