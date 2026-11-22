#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/frame_bitrate.c; fi
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTS=OFF -DBUILD_KVAZAAR_BINARY=OFF -DGIT_SUBMODULE=OFF -DUSE_CRYPTO=OFF
cmake --build .task-build --target kvazaar -j2
cc -std=c11 -Isrc task_support/baseline.c .task-build/libkvazaar.a -pthread -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -Isrc -I. tests/frame_bitrate.c task_support/driver.c .task-build/libkvazaar.a -pthread -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in credit pipeline compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
