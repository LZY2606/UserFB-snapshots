#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
mkdir -p .task-build
if [[ $mode == target ]]; then test -s test/encoder/task_au_limit.c; fi
cmake -S task_support -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release >.task-build/config.log 2>&1
cmake --build .task-build/cmake --target libxaacenc -j 4 >.task-build/build.log 2>&1
cc -std=c99 -O1 -I. -Icommon -Iencoder -Iencoder/drc_src task_support/baseline.c .task-build/cmake/libxaacenc.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cmake --build .task-build/cmake --target libxaacdec -j 4 >.task-build/decoder-build.log 2>&1
  cc -std=c99 -O1 -I. -Icommon -Iencoder -Iencoder/drc_src -Idecoder -Idecoder/drc_src test/encoder/task_au_limit.c task_support/driver.c .task-build/cmake/libxaacenc.a .task-build/cmake/libxaacdec.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capacity history compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
