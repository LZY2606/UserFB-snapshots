#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/tsbpd_reference.cpp; fi
mkdir -p .task-build
bash task_support/build.sh >.task-build/build.log 2>&1 || { tail -n 18 .task-build/build.log >&2; exit 1; }
bash task_support/compile.sh .task-build/baseline task_support/baseline.cpp
if [[ $mode == target ]]; then
  bash task_support/compile.sh .task-build/target task_support/driver.c tests/tsbpd_reference.cpp
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in drift handoff delivery; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
