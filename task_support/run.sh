#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/seek_index.c; fi
mkdir -p .task-build
cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release >.task-build/config.log 2>&1
cmake --build .task-build/cmake --target dumb -j 4 >.task-build/build.log 2>&1
cc -std=c99 -O1 -I. -Iinclude -Itask_support task_support/baseline.c .task-build/cmake/libdumb.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. -Iinclude -Itask_support tests/seek_index.c task_support/driver.c .task-build/cmake/libdumb.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pcm clock ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
