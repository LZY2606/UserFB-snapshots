#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in include/ozz/animation/offline/clip_skin_bounds_builder.h; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s test/clip_skin_bounds_test.cpp; fi
flag=OFF
[[ $mode != target ]] || flag=ON
cmake -S task_support -B .task-build -DTASK_TARGET="$flag" -DCMAKE_BUILD_TYPE=Release -DFETCHCONTENT_FULLY_DISCONNECTED=ON >.task-config.log 2>&1 || { tail -n 25 .task-config.log; exit 1; }
cmake --build .task-build --target task_baseline -j 4 >.task-build.log 2>&1 || { tail -n 25 .task-build.log; exit 1; }
if [[ $mode == target ]]; then
  find .task-build/CMakeFiles/task_target.dir -type f -name "*.o" -delete
cmake --build .task-build --target task_target -j 4 >>.task-build.log 2>&1 || { tail -n 25 .task-build.log; exit 1; }
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in bounds tightness integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_baseline
