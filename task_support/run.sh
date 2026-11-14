#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/test_edit_plan.c; fi
mkdir -p .task-build
cc -std=c11 -O1 -I. -c task_support/upstream.c -o .task-build/upstream.o
cc -std=c11 -O1 -I. task_support/baseline.c .task-build/upstream.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -O1 -I. task_support/driver.c checks/test_edit_plan.c .task-build/upstream.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in equivalence locality ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
