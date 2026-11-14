#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/splat_target.c; fi
mkdir -p .task-build
objects=()
for source in src/*.c; do
 object=.task-build/$(basename "$source" .c).o
 cc -std=c99 -O2 -Wno-unused-function -Iinclude -Isrc -c "$source" -o "$object"
 objects+=("$object")
done
ar rcs .task-build/libTinyGL.a "${objects[@]}"
cc -std=c99 -O2 -Iinclude task_support/baseline.c .task-build/libTinyGL.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Iinclude task_support/driver.c tests/splat_target.c .task-build/libTinyGL.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in projection compositing tiles; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
