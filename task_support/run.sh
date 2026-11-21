#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/bwf_join_test.c; fi
bash task_support/build.sh "$mode"
if [[ $mode == target ]]; then
  objects=(.task-build/dr_wav.o); for source in tools/*.c; do [[ -f "$source" ]] || continue; [[ $source == tools/bwf_join.c ]] && continue; objects+=(".task-build/production-$(basename "$source" .c).o"); done; cc -std=c99 -O2 -I. -Itask_support tests/bwf_join_test.c task_support/driver.c "${objects[@]}" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline metadata errors; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
