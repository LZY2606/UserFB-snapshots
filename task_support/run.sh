#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/phase_overlap_contract.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/codec2 -DBUILD_SHARED_LIBS=OFF -DLPCNET=OFF -DUNITTEST=OFF -DCMAKE_BUILD_TYPE=Release > .task-build/configure.log 2>&1
cmake --build .task-build/codec2 --target codec2 -j 2 > .task-build/build.log 2>&1
cc -std=c99 -O2 -I. -Isrc -I.task-build/codec2 task_support/baseline.c .task-build/codec2/src/libcodec2.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cmake -S . -B .task-build/observed -DBUILD_SHARED_LIBS=OFF -DLPCNET=OFF -DUNITTEST=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DTASK_OBSERVE > .task-build/observe-configure.log 2>&1
cmake --build .task-build/observed --target codec2 -j 2 > .task-build/observe-build.log 2>&1
cc -std=c99 -O2 -DTASK_OBSERVE -I. -Isrc -I.task-build/observed tests/phase_overlap_contract.c task_support/driver.c .task-build/observed/src/libcodec2.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in phase overlap stream; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
