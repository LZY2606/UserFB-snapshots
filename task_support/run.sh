#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/visibility_gradient.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++17 -Wno-narrowing -O2 -I. task_support/baseline.cpp -o .task-build/baseline
${CXX:-c++} -std=c++17 -Wno-narrowing -O2 -I. -Dmain=smallpt_legacy_main -c smallpt.cpp -o .task-build/legacy.o
if [[ $mode == target ]]; then
  test -s visibility_gradient.hpp
test -s visibility_gradient.cpp
${CXX:-c++} -std=c++17 -O2 -I. -c visibility_gradient.cpp -o .task-build/gradient.o
${CXX:-c++} -std=c++17 -O2 -I. -x c++ task_support/driver.c tests/visibility_gradient.cpp -x none .task-build/legacy.o .task-build/gradient.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in radiometry boundary render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
