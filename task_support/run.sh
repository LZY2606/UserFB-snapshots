#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/fmp_contract.cpp; fi
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build --target baseline -j 4
if [[ $mode == target ]]; then
  c++ -std=c++11 -I. -Icommon -x c++ tests/fmp_contract.cpp task_support/driver.c -x none .task-build/libfwcore.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in items addresses compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
