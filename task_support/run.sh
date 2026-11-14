#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_metric.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++20 -O0 -DPMP_SCALAR_TYPE_64 -I. -Isrc -Iexternal/eigen-5.0.1 tests/task_metric.cpp task_support/driver.c .task-build/libproduction.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in metric_direction metric_transport metric_compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
