#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/latm_decode.c; fi
cmake -S . -B .task-build -DBUILD_SHARED_LIBS=OFF -DFAAD_BUILD_CLI=OFF -DCMAKE_BUILD_TYPE=Release >.task-build-config.log 2>&1
cmake --build .task-build --target faad -j 4 >.task-build-compile.log 2>&1
cc -std=c99 -I. -Iinclude task_support/baseline.c .task-build/libfaad.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinclude task_support/driver.c tests/latm_*.c .task-build/libfaad.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in transport continuity buffers; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
