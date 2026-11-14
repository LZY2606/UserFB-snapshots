#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/orient_solid.c; fi
mkdir -p .task-build build
cc -std=c99 -O2 -I. task_support/baseline.c -lm -o .task-build/baseline
cc -std=c99 -O2 -I. -Itest test/test_shapes.c test/console-colors.c -lm -o .task-build/old-shapes
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. task_support/driver.c test/orient_solid.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology nesting publication; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/old-shapes
