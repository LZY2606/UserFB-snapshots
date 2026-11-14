#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/resample_regression.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. -Itask_support -Ilibtwolame libtwolame/*.c task_support/baseline.c -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s libtwolame/resample.c
  test -s libtwolame/resample.h
  cc -std=c99 -O2 -I. -Itask_support -Ilibtwolame libtwolame/*.c task_support/driver.c tests/resample_regression.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clock filter frontends legacy; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
