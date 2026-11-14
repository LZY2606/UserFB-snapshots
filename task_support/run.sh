#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/flips.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DOVM_ENABLE_APPLICATIONS=OFF -DOVM_ENABLE_UNITTESTS=OFF -DOVM_ENABLE_EXAMPLES=OFF -DOVM_BUILD_DOCUMENTATION=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release > .task-build/configure.log 2>&1
cmake --build .task-build/lib --target OpenVolumeMesh -j2 > .task-build/build.log 2>&1
c++ -std=c++17 -I. -Isrc -I.task-build/lib/src task_support/baseline.cpp .task-build/lib/Build/lib/libOpenVolumeMesh.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -I. -Isrc -I.task-build/lib/src -x c++ task_support/driver.c task_tests/flips.cpp -x none .task-build/lib/Build/lib/libOpenVolumeMesh.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in cavity geometry state; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
