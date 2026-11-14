#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/dropmsg.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++11 -pthread -I. -Isrtcore -I.task-build task_support/driver.c task_tests/dropmsg.cpp .task-build/libsrt.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in buffer wire recovery api; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++11 -pthread -Isrtcore -I.task-build task_support/baseline.cpp .task-build/libsrt.a -o .task-build/baseline
.task-build/baseline
