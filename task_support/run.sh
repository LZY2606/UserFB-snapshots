#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/poses.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DMESHOPT_BUILD_DEMO=OFF -DMESHOPT_BUILD_GLTFPACK=OFF -DMESHOPT_BUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release > .task-build/configure.log 2>&1
cmake --build .task-build/lib --target meshoptimizer -j2 > .task-build/build.log 2>&1
c++ -std=c++11 -I. -Isrc task_support/baseline.cpp .task-build/lib/libmeshoptimizer.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -I. -Isrc -x c++ task_support/driver.c task_tests/poses.cpp -x none .task-build/lib/libmeshoptimizer.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in ensemble persistence identity; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
