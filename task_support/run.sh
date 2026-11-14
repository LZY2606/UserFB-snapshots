#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/geodesic_network.cpp; fi
mkdir -p .task-build
cmake -S task_support -B .task-build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS="-O0 -g0"
cmake --build .task-build --target geometry-central -j 4
c++ -std=c++17 -O0 -I. -x c++ -c task_support/driver.c -o .task-build/driver.o
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -I. -Iinclude -I.deps/eigen task_tests/geodesic_network.cpp .task-build/driver.o .task-build/gc/src/libgeometry-central.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in chart network rewind; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cmake --build .task-build --target task_baseline -j 4
.task-build/task_baseline
