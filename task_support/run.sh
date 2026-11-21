#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s unit_tests/test_608_resume.c; fi
mkdir -p .task-build/obj
rm -f .task-build/obj/*.o .task-build/libcaption.a
cp src/eia608_from_utf8.c.cached .task-build/eia608_from_utf8.c
for file in src/*.c .task-build/eia608_from_utf8.c; do
  case "$file" in src/dvtcc.c|src/eia608_from_utf8.c) continue ;; esac
  cc -std=c99 -D__STDC_CONSTANT_MACROS -I. -Icaption -c "$file" -o ".task-build/obj/$(basename "${file%.c}").o"
done
ar rcs .task-build/libcaption.a .task-build/obj/*.o
cc -std=c99 -I. -Icaption task_support/baseline.c .task-build/libcaption.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Icaption unit_tests/test_608_resume.c task_support/driver.c .task-build/libcaption.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in routing checkpoint compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
