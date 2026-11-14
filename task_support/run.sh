#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/log_depth.c; fi
mkdir -p .task-build
cc -std=c99 -O1 -I. -Iinclude -Isrc -c src/*.c
ar rcs .task-build/libtgl.a ./*.o
rm -f ./*.o
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. -Iinclude -Isrc task_support/driver.c task_tests/log_depth.c .task-build/libtgl.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry visibility lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O1 -I. -Iinclude -Isrc task_support/baseline.c .task-build/libtgl.a -lm -o .task-build/baseline
.task-build/baseline
