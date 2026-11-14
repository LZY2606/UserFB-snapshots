#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
mode=${1:-baseline}
cc -std=c99 -IInclude Source/*.c support/baseline.c -lm -o "$build/baseline"
"$build/baseline"
if [[ "$mode" == target ]]; then
  cc -std=c99 -Isupport -c support/runner.c -o "$build/runner.o"
  cc -std=c99 -IInclude Source/*.c -Isupport support/target.c "$build/runner.o" -lm -o "$build/target"
  "$build/target" | tee "$build/target.log"
  for group in quality constraints storage; do
    grep -Eq "^PASS $group/.+" "$build/target.log"
  done
  grep -Eq '^TARGET COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$build/target.log"
fi
