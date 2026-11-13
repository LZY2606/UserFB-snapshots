#!/usr/bin/env bash
set -euo pipefail
build=$(mktemp -d 2>/dev/null || {
  root="${TMPDIR:-/tmp}/cpbuild-$$-$RANDOM"
  mkdir -p "$root"
  echo "$root"
})
trap 'rm -rf "$build"' EXIT
mode=${1:-baseline}
cc -std=c99 -Iinclude src/*.c support/baseline.c -lm -pthread -o "$build/baseline"
"$build/baseline"
if [[ "$mode" == target ]]; then
  cc -std=c99 -Isupport -c support/runner.c -o "$build/runner.o"
  cc -std=c99 -Iinclude -Isupport src/*.c support/target.c "$build/runner.o" -lm -pthread -o "$build/target"
  "$build/target" | tee "$build/target.log"
  for group in contacts joints sleep; do
    grep -Eq "^PASS $group/.+" "$build/target.log"
  done
  grep -Eq '^TARGET COMPLETE cases=[1-9][0-9]* checks=[1-9][0-9]*$' "$build/target.log"
fi
