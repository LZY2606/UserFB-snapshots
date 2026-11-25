#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/slice_concealment.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  "${CC:-cc}" -std=c99 -O2 -I . -I task_support -I include -I libmpeg2 task_support/driver.c tests/slice_concealment.c .task-build/libmpeg2.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coverage pixels reorder lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
"${CC:-cc}" -std=c99 -O2 -I task_support -I include task_support/baseline.c .task-build/libmpeg2.a -o .task-build/baseline
.task-build/baseline
