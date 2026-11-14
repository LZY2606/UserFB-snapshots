#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/task_cut_window.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c11 -D_GNU_SOURCE -O1 -Isrc -Isrc/framework -I.task-build/include -I. src/tests/task_cut_window.c task_support/driver.c .task-build/libmlt.a -pthread -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in window graph frames; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c11 -D_GNU_SOURCE -O1 -Isrc -I.task-build/include task_support/baseline.c .task-build/libmlt.a -pthread -lm -o .task-build/baseline
.task-build/baseline
