#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
[ -s task_tests/target.cpp ] || { echo 'missing task_tests/target.cpp' >&2; exit 2; }
build=$(mktemp -d "${TMPDIR:-/tmp}/check-target.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM
cxx=${CXX:-c++}
"$cxx" -std=c++11 -O2 -I. -Itask_support -Iexamples/common task_support/target_main.cpp task_tests/target.cpp -o "$build/tests"
status=0
"$build/tests" > "$build/target.stdout" 2> "$build/target.stderr" || status=$?
cat "$build/target.stdout"
cat "$build/target.stderr" >&2
[ "$status" -eq 0 ] || exit "$status"
awk -f task_support/check-output.awk "$build/target.stdout"
bash task_support/baseline.sh
printf '%s\n' 'TUBE_TARGET_COMPLETE'
