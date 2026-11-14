#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/offline_task.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  cc -std=c11 -O0 -g -I. -I task_support task_support/driver.c tests/offline_task.c task_support/basic_fixture.c .task-build/libbacktrace.a -o .task-build/target
  output_log=.task-build/target.log
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in modules callbacks lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
