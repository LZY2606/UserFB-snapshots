#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build=$(mktemp -d "${TMPDIR:-/tmp}/check-baseline.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
cxx=${CXX:-c++}
"$cxx" -std=c++11 -O2 -I. -Iexamples/common task_support/baseline.cpp -o "$build/start"
"$build/start"
"$cxx" -std=c++11 -O2 -I. test/regression/possible-accuracy-problem-30/main.cc -o "$build/old"
for mode in normal perturbed; do
  if [ "$mode" = normal ]; then "$build/old" > "$build/$mode.log"; else "$build/old" perturb > "$build/$mode.log"; fi
  cat "$build/$mode.log"
  grep -qx 'We have the expected result' "$build/$mode.log"
  if grep -q 'wrong result' "$build/$mode.log"; then exit 5; fi
done
printf '%s\n' 'NANORT_LEGACY_COMPLETE modes=2'
