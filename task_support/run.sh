#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/timed_events.c; fi
cmake -S . -B .task-build/lib -DBUILD_SHARED=OFF -DBUILD_STATIC=ON -DWITH_UNIT_TESTS=OFF -DLIBXMP_DISABLE_DEPACKERS=ON -DLIBXMP_DISABLE_PROWIZARD=ON
cmake --build .task-build/lib --target xmp_static --parallel 4
: > .task-build/all_tests.c
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinclude -Isrc -I.task-build -DLIBXMP_STATIC task_support/driver.c tests/timed_events.c test-dev/simple_module.c test-dev/util.c .task-build/lib/libxmp.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline voices stream; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -Iinclude checks/baseline.c .task-build/lib/libxmp.a -lm -o .task-build/baseline
.task-build/baseline
bash checks/old-tests.sh
