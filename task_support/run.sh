#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/deadline_tcp.cpp; fi
mkdir -p .task-build
make -s -j4 -f task_support/production.mk
if [[ $mode == target ]]; then
  bash task_support/link.sh .task-build/target task_support/driver.c task_tests/deadline_tcp.cpp
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in framing deadlines lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  bash task_support/link.sh .task-build/baseline task_support/baseline.cpp
  .task-build/baseline
fi
