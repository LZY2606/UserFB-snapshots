#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/retained.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O2 -I. -Isrc task_support/baseline.cpp src/TriangleDrawer.cpp src/Triangle.cpp src/Graphics3D.cpp src/LineDrawer.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -O2 -I. -Isrc task_tests/retained.cpp task_support/driver.c src/RetainedRenderer.cpp src/TriangleDrawer.cpp src/Triangle.cpp src/Graphics3D.cpp src/LineDrawer.cpp -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in damage scissor locality; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
