#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/inverse_design.cpp; fi
bash baseline/build.sh
if [[ $mode == target ]]; then
  "${CXX:-c++}" -std=c++17 -O1 -I. -IlibShapeOp/src -Ivendor/eigen task_support/driver.c tests/inverse_design.cpp .task-build/libshapeop.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in equilibrium adjoint lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash baseline/run.sh
