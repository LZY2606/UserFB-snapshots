#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/motion_cases.cpp; fi
mkdir -p .task-build
c++ -std=c++11 -O2 -I. -DNANORT_ENABLE_SERIALIZATION task_support/baseline.cpp -o .task-build/baseline
c++ -std=c++11 -O2 -I. test/regression/possible-accuracy-problem-30/main.cc -o .task-build/upstream-regression
if [[ $mode == target ]]; then
  test -s nanort_motion.h
c++ -std=c++11 -O2 -I. -DNANORT_ENABLE_SERIALIZATION -include nanort_motion.h tests/motion_cases.cpp task_support/driver.c -o .task-build/target
c++ -std=c++11 -O2 -I. -DNANORT_ENABLE_SERIALIZATION -include nanort_motion.h examples/motion_render.cpp -o .task-build/motion-demo
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in deformation bounds persistence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == target ]]; then
  .task-build/motion-demo .task-build/motion.ppm
  test -s .task-build/motion.ppm
fi
.task-build/baseline
.task-build/upstream-regression
