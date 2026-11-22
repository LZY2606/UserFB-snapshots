#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/reference_feedback.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  "${CC:-cc}" -std=c99 -O2 -I . -I .task-build/x264 -I task_support task_support/driver.c tests/reference_feedback.c .task-build/x264/libx264.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in graph lists pipeline compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"${CC:-cc}" -std=c99 -O2 -I . -I .task-build/x264 task_support/baseline.c .task-build/x264/libx264.a -lm -o .task-build/baseline
.task-build/baseline
