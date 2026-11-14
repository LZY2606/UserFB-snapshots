#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/hybrid_plan_test.c; fi
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libwavpack.a
for task_source in src/*.c; do
  cc -std=c99 -O2 -include strings.h -D_POSIX_C_SOURCE=200809L -DENABLE_THREADS -DENABLE_DSD -Iinclude -Isrc -I. -pthread -c "$task_source" -o ".task-build/$(basename "${task_source%.c}").o"
done
ar rcs .task-build/libwavpack.a .task-build/*.o
cc -std=c99 -O2 -include strings.h -Iinclude task_support/baseline.c .task-build/libwavpack.a -pthread -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -include strings.h -D_POSIX_C_SOURCE=200809L -Iinclude -Isrc -I. task_support/driver.c tests/hybrid_plan_test.c .task-build/libwavpack.a -pthread -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in schedule budgets compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
