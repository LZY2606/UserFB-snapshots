#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/forced_idr.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/uvg -DUVG_BUILD_TESTS=OFF -DUVG_BUILD_SHARED=OFF -DUVG_BUILD_EXECUTABLE=OFF -DUVG_ENABLE_CCACHE=OFF -DUVG_SYNC_SUBMODULES=OFF -DUVG_ENABLE_AVX2=OFF -DUVG_ENABLE_SSE2=OFF -DUVG_ENABLE_SSE41=OFF -DUVG_ENABLE_SSE42=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/uvg --target libuvg266_static -j 4
cc -std=c11 -O2 -I. -Isrc -I.task-build/uvg/src -DUVG_BIT_DEPTH=8 task_support/baseline.c .task-build/uvg/lib/libuvg266.a -lm -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -O2 -I. -Isrc -I.task-build/uvg/src -DUVG_BIT_DEPTH=8 task_support/driver.c tests/forced_idr.c .task-build/uvg/lib/libuvg266.a -lm -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in boundary references pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
