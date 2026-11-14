#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/gather_contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  "${CC:-cc}" -std=c99 -O0 -DHAVE_STRING_H=1 -I. -Ilibudis86 -Itask_support/generated -Itask_support/generated/libudis86 libudis86/*.c task_support/generated/libudis86/itab.c tests/gather_contract.c task_support/driver.c -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in width_map vsib compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/libcheck
echo LIBCHECK_OK
.task-build/baseline
