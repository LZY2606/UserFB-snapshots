#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/microflake.cpp; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
 for file in volume/sggx.hpp volume/sggx.cpp volume/sparse_medium.hpp volume/sparse_medium.cpp examples/microflake_headless.cpp; do test -s "$file"; done
 c++ -std=c++17 -O2 -I. -Icpp-smallpt/cpp-smallpt/src -include task_support/portable.hpp -include volume/sggx.hpp -include volume/sparse_medium.hpp volume/sggx.cpp volume/sparse_medium.cpp task_support/surface_baseline.cpp -o .task-build/baseline
else
 c++ -std=c++17 -O2 -I. -Icpp-smallpt/cpp-smallpt/src -include task_support/portable.hpp task_support/surface_baseline.cpp -o .task-build/baseline
fi
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Icpp-smallpt/cpp-smallpt/src -include task_support/portable.hpp -include volume/sggx.hpp -include volume/sparse_medium.hpp volume/sggx.cpp volume/sparse_medium.cpp tests/microflake.cpp task_support/driver.c -o .task-build/target
c++ -std=c++17 -O2 -I. -Icpp-smallpt/cpp-smallpt/src -include task_support/portable.hpp -include volume/sggx.hpp -include volume/sparse_medium.hpp volume/sggx.cpp volume/sparse_medium.cpp examples/microflake_headless.cpp -o .task-build/microflake_example
mkdir -p .task-build/microflake
rm -f .task-build/microflake/axis.ppm .task-build/microflake/rotated.ppm
.task-build/microflake_example .task-build/microflake
test -s .task-build/microflake/axis.ppm
test -s .task-build/microflake/rotated.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in tensor flight transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
