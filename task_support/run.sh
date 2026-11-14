#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/sequence_switch.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -include stdio.h -include stddef.h -std=c99 -O2 -I. -Itask_support tests/sequence_switch.c task_support/driver.c .task-build/plm.o .task-build/fixture.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in planes clock stream; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then .task-build/baseline normal; else .task-build/baseline; fi
