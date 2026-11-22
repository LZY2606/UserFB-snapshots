#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/h2_contract.c; fi
mkdir -p .task-build
make libsbc BUILD_DIR=.task-build/obj BIN_DIR=.task-build/bin > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
cc -std=c11 -O2 -Iinclude task_baseline.c .task-build/bin/libsbc.a -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -O2 -I. -Iinclude task_support/driver.c tests/h2_contract.c .task-build/obj/src/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Framing Prediction Recovery Backpressure; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
