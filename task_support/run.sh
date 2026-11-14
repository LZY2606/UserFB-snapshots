#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/heap_recovery_contract.c; fi
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libmemwatch.a
for source_file in *.c; do [[ "$source_file" == test.c ]] && continue; cc -std=c99 -O1 -DMEMWATCH -DMEMWATCH_STDIO -DMW_HEAP_TESTING -I. -c "$source_file" -o ".task-build/${source_file%.c}.o"; done
ar -rcs .task-build/libmemwatch.a .task-build/*.o
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -DMEMWATCH -DMEMWATCH_STDIO -DMW_HEAP_TESTING -I. task_support/driver.c test/heap_recovery_contract.c .task-build/libmemwatch.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ledger repair lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O1 -DMEMWATCH -DMEMWATCH_STDIO -I. task_support/baseline.c .task-build/libmemwatch.a -o .task-build/baseline
.task-build/baseline
