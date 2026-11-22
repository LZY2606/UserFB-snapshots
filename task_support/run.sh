#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/picture_boundary.cc; fi
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DENABLE_SDL=OFF -DENABLE_SIMD=OFF -DENABLE_DECODER=OFF -DENABLE_ENCODER=OFF -DENABLE_SHERLOCK265=OFF -DENABLE_INTERNAL_DEVELOPMENT_TOOLS=OFF
cmake --build .task-build --target de265 -j2
c++ -std=c++17 -DLIBDE265_STATIC_BUILD -I. -I.task-build task_support/baseline.cc .task-build/libde265/libde265.a -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -DLIBDE265_STATIC_BUILD -I. -I.task-build tests/picture_boundary.cc task_support/driver.c .task-build/libde265/libde265.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in queue reorder resume; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
