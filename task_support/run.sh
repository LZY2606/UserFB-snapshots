#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/beat_clock_test.cpp; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  flags=$(cat task_support/flags.txt)
c++ $flags -std=c++17 -include src/core/chuck_beat.h -x c++ tests/beat_clock_test.cpp task_support/driver.c -x none .task-build/src/core/chuck_beat.o .task-build/libchuck.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timing ramp lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
