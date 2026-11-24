#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s testing/hiz_target.cpp; fi
mkdir -p .task-build
bash task_support/amalgamate.sh > .task-build/portablegl.h
c++ -std=c++17 -O2 -I.task-build -I. task_support/baseline.cpp -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I.task-build -I. task_support/driver.c task_support/implementation.cpp testing/hiz_target.cpp -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in bounds coherence fallback; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
if [[ $mode == target ]]; then bash task_support/old.sh; fi
