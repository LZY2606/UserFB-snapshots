#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/normal_moments.cpp; fi
mkdir -p .task-build
cxx="${CXX:-c++}"
sources=(cpurast/*.cpp)
for source_file in shaders/*.cpp; do
  if test -f "$source_file"; then sources+=("$source_file"); fi
done
"$cxx" -std=c++17 -O1 -Icpurast -Ishaders -iquote . "${sources[@]}" task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s cpurast/normal_moments.hpp
test -s cpurast/normal_moments.cpp
test -s shaders/lean_shader.hpp
test -s shaders/lean_shader.cpp
test -s examples/lean_cpu.cpp
"$cxx" -std=c++17 -O1 -Icpurast -Ishaders -iquote . -include cpurast/normal_moments.hpp -include shaders/lean_shader.hpp "${sources[@]}" -x c++ task_support/driver.c task_tests/normal_moments.cpp -o .task-build/target
"$cxx" -std=c++17 -O1 -Icpurast -Ishaders -iquote . -include cpurast/normal_moments.hpp -include shaders/lean_shader.hpp "${sources[@]}" examples/lean_cpu.cpp -o .task-build/lean_cpu
rm -f .task-build/lean.ppm
.task-build/lean_cpu .task-build/lean.ppm
test -s .task-build/lean.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pyramid footprint shading; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
