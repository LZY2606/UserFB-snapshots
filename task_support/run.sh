#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in inc/spvm/forward.h; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s task_tests/forward.c; fi
mkdir -p .task-build
for unit in src/*.c src/ext/*.c; do
  obj=.task-build/$(basename "${unit%.c}").o
  if [[ ! -f "$obj" || "$unit" -nt "$obj" ]]; then cc -std=c99 -D_DEFAULT_SOURCE -O0 -g -Iinc -c "$unit" -o "$obj"; fi
done
ar rcs .task-build/libspvm.a .task-build/*.o
if [[ $mode == target ]]; then
  cc -std=c99 -D_DEFAULT_SOURCE -O0 -I. -Iinc task_support/driver.c task_tests/forward.c .task-build/libspvm.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in arithmetic aliases control; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -D_DEFAULT_SOURCE -O0 -Iinc checks/old_shader.c .task-build/libspvm.a -lm -o .task-build/baseline
.task-build/baseline
printf 'BASELINE_COMPLETE\n'
