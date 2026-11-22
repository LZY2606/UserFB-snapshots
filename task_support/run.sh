#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/checkpoint_test.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DBUILD_TESTING=OFF -DLIBSAMPLERATE_EXAMPLES=OFF -DLIBSAMPLERATE_INSTALL=OFF -DBUILD_SHARED_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_PkgConfig=TRUE
cmake --build .task-build/cmake --target samplerate -j 2
cc -std=c99 -Iinclude -Isrc task_support/baseline.c .task-build/cmake/src/libsamplerate.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinclude -Isrc tests/checkpoint_test.c task_support/driver.c .task-build/cmake/src/libsamplerate.a -lm -o .task-build/target
  if [[ -s tests/checkpoint_child.c ]]; then cc -std=c99 -I. -Iinclude -Isrc tests/checkpoint_child.c .task-build/cmake/src/libsamplerate.a -lm -o .task-build/checkpoint-child; fi
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in format resume callback; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
