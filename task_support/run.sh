#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bash task_support/build.sh "$build"
if [ "${1:-baseline}" = target ]; then
 test -s unit_target.c
 ${CC:-cc} -std=c11 -O1 -I. -Itask_support unit_target.c task_support/driver.c "$build/libsource.a" -lm -o "$build/target"
 "$build/target" > "$build/target.log"
 cat "$build/target.log"
 node task_support/verify.cjs "$build/target.log"
fi
"$build/baseline"
if test -x "$build/old-tests"; then (cd test; "$build/old-tests"); fi
echo BASELINE_OK
