#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/motion.cpp; fi
mkdir -p .task-build
cxx="${CXX:-c++}"
sources=(src/*.cpp)
"$cxx" -std=c++17 -O1 -Isrc -iquote . "${sources[@]}" task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/egl_motion.h
test -s examples/motion_cpu.cpp
"$cxx" -std=c++17 -O1 -Isrc -iquote . -include src/egl_motion.h "${sources[@]}" task_support/driver.c task_tests/motion.cpp -o .task-build/target
"$cxx" -std=c++17 -O1 -Isrc -iquote . -include src/egl_motion.h "${sources[@]}" examples/motion_cpu.cpp -o .task-build/motion_cpu
rm -f .task-build/motion.ppm
.task-build/motion_cpu .task-build/motion.ppm
test -s .task-build/motion.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in rows clipping cache; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
