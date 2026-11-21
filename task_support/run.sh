#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/cueselect_test.c; fi
mkdir -p .task-build/obj
rm -f .task-build/obj/*.o .task-build/libwebvtt.a
for file in src/libwebvtt/*.c; do
  cc -std=c99 -DWEBVTT_STATIC=1 -Iinclude -c "$file" -o ".task-build/obj/$(basename "${file%.c}").o"
done
ar rcs .task-build/libwebvtt.a .task-build/obj/*.o
cc -std=c99 -DWEBVTT_STATIC=1 -I. -Iinclude task_support/baseline.c .task-build/libwebvtt.a -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -DWEBVTT_STATIC=1 -I. -Iinclude test/cueselect_test.c task_support/driver.c .task-build/libwebvtt.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in selection anchors transaction; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
