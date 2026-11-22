#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/clocked.c; fi
mkdir -p .task-build
find .task-build -maxdepth 1 -type f \( -name "*.o" -o -name "*.a" \) -delete
for source_path in src/*.c; do
  object_path=".task-build/$(basename "${source_path%.c}").o"
  cc -std=c99 -O0 -Isrc -I. -c "$source_path" -o "$object_path"
done
ar rcs .task-build/libltc.a .task-build/*.o
if [[ $mode == target ]]; then
  cc -std=c99 -O0 -Isrc -I. task_support/driver.c tests/clocked.c .task-build/libltc.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in measured_grid relabel queue_pairing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O0 -Isrc -I. task_support/baseline.c .task-build/libltc.a -lm -o .task-build/baseline
.task-build/baseline
