#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/volume.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -Wno-unknown-pragmas -I. -Isrc/renderer -c src/renderer/*.cpp
ar rcs .task-build/renderer.a ./*.o
rm ./*.o
c++ -std=c++17 -O2 -Wno-unknown-pragmas -I. -Isrc/renderer task_support/baseline.cpp .task-build/renderer.a -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -Wno-unknown-pragmas -I. -Isrc/renderer task_tests/volume.cpp task_support/driver.c .task-build/renderer.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture overlap lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
