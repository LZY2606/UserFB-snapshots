#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/description_runs_test.cpp; fi
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="$(uname -m)" -DBUILD_APPS=ON -DBUILD_SHARED_LIBRARY=OFF
cmake --build .task-build --target task_fixture mp4fragment -j 4
if [[ $mode == target ]]; then
  cmake -E rm -f .task-build/task_support/CMakeFiles/task_target.dir/description_runs_test.cpp.o
cmake --build .task-build --target task_target -j 4
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in layout reader compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/task_support/task_fixture create .task-build/input.mp4
.task-build/task_support/task_fixture input .task-build/input.mp4
.task-build/mp4fragment --quiet --fragment-duration 4000 --index .task-build/input.mp4 .task-build/output.mp4
.task-build/task_support/task_fixture fragment .task-build/output.mp4
