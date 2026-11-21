#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_hinted_edits.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DMP4V2_BUILD_SHARED=OFF > .task-build/build.log 2>&1
cmake --build .task-build/lib --target mp4v2 -j 4 >> .task-build/build.log 2>&1
c++ -std=c++11 -DMP4V2_USE_STATIC_LIB -Iinclude task_support/baseline.cpp .task-build/lib/libmp4v2.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/copy_hinted_edits.cpp
test -s task_support/require_hinted_api.cpp
c++ -std=c++11 -DMP4V2_USE_STATIC_LIB -Iinclude task_support/require_hinted_api.cpp .task-build/lib/libmp4v2.a -o .task-build/required-api
c++ -std=c++11 -DMP4V2_USE_STATIC_LIB -I. -Iinclude tests/task_hinted_edits.cpp task_support/driver.c .task-build/lib/libmp4v2.a -o .task-build/target
c++ -std=c++11 -DMP4V2_USE_STATIC_LIB -Iinclude examples/copy_hinted_edits.cpp .task-build/lib/libmp4v2.a -o .task-build/example
.task-build/baseline normal
rm -f .task-build/required-api-output.mp4
.task-build/required-api .task-build/paired-source.mp4 .task-build/required-api-output.mp4
rm -f .task-build/example-output.mp4
.task-build/example .task-build/paired-source.mp4 .task-build/example-output.mp4
test -s .task-build/example-output.mp4
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in occurrence clock reject_compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline normal
if [[ $mode == baseline ]]; then .task-build/baseline gap; fi
