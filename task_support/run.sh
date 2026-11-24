#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/alloc_failure_contract.c; fi
mkdir -p .task-build
rm -f .task-build/*.o .task-build/libtheft.a
awk -f scripts/mk_bits_lut > .task-build/bits_lut.h
for source_file in src/*.c; do cc -std=c99 -O1 -D_POSIX_C_SOURCE=199309L -D_C99_SOURCE -Iinc -Isrc -Ivendor -I.task-build -c "$source_file" -o ".task-build/$(basename "${source_file%.c}").o"; done
ar -rcs .task-build/libtheft.a .task-build/*.o
if [[ $mode == target ]]; then
  bash task_support/fault-build.sh; cc -std=c99 -O1 -D_POSIX_C_SOURCE=199309L -D_C99_SOURCE -I. -Iinc -Isrc -Ivendor -I.task-build task_support/driver.c test/alloc_failure_contract.c test/alloc_fault.c .task-build/fault/libtheft.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in init trials shrinking; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
for baseline_name in shrink fork; do cc -std=c99 -O1 -D_POSIX_C_SOURCE=199309L -D_C99_SOURCE -Iinc -Isrc -Ivendor -I.task-build "task_support/baseline_${baseline_name}.c" .task-build/libtheft.a -lm -o ".task-build/baseline_${baseline_name}"; ".task-build/baseline_${baseline_name}"; done
