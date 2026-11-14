#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/seek_origin_test.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -Wall -Wextra -I. -Iinclude task_support/driver.c tests/seek_origin_test.c .task-build/libwavpack.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in origin lanes restart; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -Wall -Wextra -Iinclude task_support/baseline.c .task-build/libwavpack.a -lm -o .task-build/baseline
.task-build/baseline
.task-build/wvtest --default --short --no-extras --no-floats --no-lossy --no-speeds > .task-build/upstream-tests.log 2>&1
grep -q "^all tests pass$" .task-build/upstream-tests.log
echo WAVPACK_UPSTREAM_INTEGER_OK
