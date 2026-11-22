#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_retime.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED=OFF -DBUILD_UTILS=OFF > .task-build/build.log 2>&1
cmake --build .task-build/lib -j 4 >> .task-build/build.log 2>&1
if [[ $mode == target ]]; then
  c++ -std=c++11 -DMP4V2_USE_STATIC_LIB -I. -Iinclude -I.task-build/lib/include -x c++ task_support/driver.c tests/task_retime.cpp -x none .task-build/lib/libmp4v2.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in quantize state reject compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  c++ -std=c++11 -DMP4V2_USE_STATIC_LIB -I. -Iinclude -I.task-build/lib/include task_support/baseline.cpp .task-build/lib/libmp4v2.a -o .task-build/baseline
  .task-build/baseline
fi
