#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/inter_weight_contract.cc; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -O1 -I. -I.task-build/lib -I.task-build/lib/libde265 task_support/driver.c tests/inter_weight_contract.cc .task-build/lib/libde265/libde265.a -lpthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in precision lists planes; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
