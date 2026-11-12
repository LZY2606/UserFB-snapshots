#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test -s task_tests/pressure.cpp || { echo 'TARGET_FAIL missing or empty task_tests/pressure.cpp' >&2; exit 1; }
build=$(mktemp -d "${TMPDIR:-/tmp}/agg-pressure.XXXXXX")
trap 'rm -rf "$build"' EXIT
sources=(task_support/runner.cpp task_tests/pressure.cpp agg-src/src/agg_curves.cpp agg-src/src/agg_vcgen_stroke.cpp)
if test -f agg-src/src/agg_pressure_stroke.cpp; then sources+=(agg-src/src/agg_pressure_stroke.cpp); fi
"${CXX:-c++}" -std=c++17 -O2 -Iagg-src/include -Itask_support "${sources[@]}" -o "$build/pressure"
"$build/pressure" | tee "$build/result"
awk '
 /^GROUP_OK (profile|offset|joins|render) cases=[1-9][0-9]* checks=[1-9][0-9]*$/ {if(seen[$2]++)bad=1; groups++}
 /^PRESSURE_COMPLETE groups=4 cases=[1-9][0-9]* checks=[1-9][0-9]*$/ {complete++}
 END {if(bad||groups!=4||complete!=1)exit 1}
' "$build/result"
bash task_support/baseline.sh
