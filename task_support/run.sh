#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/retry_fanout.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=gnu11 -O1 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -I. -I.task-build -I.task-build/include -I.task-build/include/librist -Isrc -Iinclude -Iinclude/librist -Icontrib -Icontrib/lz4 -Icontrib/contrib_cJSON task_tests/retry_fanout.c task_support/driver.c .task-build/libtaskrist.a -lm -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fanout lifecycle wire; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
cc -std=gnu11 -O1 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -I. -I.task-build -I.task-build/include -I.task-build/include/librist -Isrc -Iinclude -Iinclude/librist -Icontrib -Icontrib/lz4 -Icontrib/contrib_cJSON task_support/baseline.c .task-build/libtaskrist.a -lm -pthread -o .task-build/baseline
.task-build/baseline
fi
