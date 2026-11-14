#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/slice_test.cpp; fi
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release > .task-build-config.log 2>&1
cmake --build .task-build --target baseline -j 4 > .task-build-build.log 2>&1
if [[ $mode == target ]]; then
  test -s src/opentimelineio/transitionSlice.cpp
rm -f .task-build/CMakeFiles/target.dir/task_support/slice_test.cpp.o .task-build/target
cmake --build .task-build --target target -j 4 >> .task-build-build.log 2>&1
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in partial retrim persistence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
