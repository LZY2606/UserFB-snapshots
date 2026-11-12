#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
b=$(mktemp -d "${TMPDIR:-/tmp}/cxx.XXXXXX")
trap 'rm -rf "$b"' EXIT
c++ -std=c++17 -O2 -I. test/regression/possible-accuracy-problem-30/main.cc -o "$b/legacy"
for mode in normal tiny-direction; do
  if [ "$mode" = normal ]; then "$b/legacy" > "$b/result"; else "$b/legacy" tiny > "$b/result"; fi
  cat "$b/result"
  test "$(grep -c '^We have the expected result$' "$b/result")" -eq 1
  if grep -q 'wrong result\|No intersection detected' "$b/result"; then exit 1; fi
  echo "LEGACY_PASS $mode"
done
echo 'NANORT_LEGACY_OK cases=2'
