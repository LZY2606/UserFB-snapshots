#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/wav_ima_tail_cases.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/lib -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DENABLE_EXTERNAL_LIBS=OFF -DENABLE_MPEG=OFF -DENABLE_CPACK=OFF -DENABLE_PACKAGE_CONFIG=OFF -DCMAKE_DISABLE_FIND_PACKAGE_Python=TRUE >.task-build/configure.log 2>&1
cmake --build .task-build/lib --target sndfile -j 2 >.task-build/build.log 2>&1
cc -std=c99 -Iinclude task_support/baseline.c .task-build/lib/libsndfile.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinclude -Itask_support tests/wav_ima_tail_cases.c task_support/driver.c .task-build/lib/libsndfile.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry fact seek; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline "$mode"
