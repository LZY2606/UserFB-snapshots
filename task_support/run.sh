#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/matrix_contract.c; fi
mkdir -p .task-build
rm -f .task-build/*.o
task_def=-UA52_MATRIX_TEST
if [[ $mode == target ]]; then task_def=-DA52_MATRIX_TEST; fi
for task_source in liba52/*.c; do
  task_part=${task_source##*/}
  ${CC:-cc} -std=c99 -O2 "$task_def" -Itask_support -Iinclude -Iliba52 -c "$task_source" -o ".task-build/${task_part%.c}.o"
done
if [[ $mode == target ]]; then
  ${CC:-cc} -std=c99 -O2 -DA52_MATRIX_TEST -I. -Iinclude -Iliba52 -Itask_support task_support/driver.c tests/matrix_contract.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in matrix overlap grouping compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
${CC:-cc} -std=c99 -O2 -Iinclude task_support/baseline.c .task-build/*.o -lm -o .task-build/baseline
.task-build/baseline
