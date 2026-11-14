#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/export_safety_cases.c; fi
bash task_support/build_core.sh
cc -std=c11 -D_DEFAULT_SOURCE -I. -Ilib/libpe/include task_support/baseline.c .task-build/pe_core.o .task-build/exports.o .task-build/error.o -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -D_DEFAULT_SOURCE -I. -Ilib/libpe/include task_support/driver.c tests/export_safety_cases.c .task-build/pe_core.o .task-build/exports.o .task-build/error.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in tables strings lifetime; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
