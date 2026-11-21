#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/curve_network.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O0 -DNANOSPLINE_HEADER_ONLY -DEIGEN_MPL2_ONLY -Iinclude -Ivendor/eigen task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/nanospline/CurveNetwork.h
test -s include/nanospline/ReparameterizedBSpline.h
test -s src/CurveNetwork.cpp
test -s src/ReparameterizedBSpline.cpp
test -s examples/curve_network.cpp
c++ -std=c++17 -O0 -DNANOSPLINE_HEADER_ONLY -DEIGEN_MPL2_ONLY -Iinclude -Ivendor/eigen -I. -include nanospline/CurveNetwork.h -include nanospline/ReparameterizedBSpline.h src/CurveNetwork.cpp src/ReparameterizedBSpline.cpp task_support/driver.c tests/curve_network.cpp -o .task-build/target
c++ -std=c++17 -O0 -DNANOSPLINE_HEADER_ONLY -DEIGEN_MPL2_ONLY -Iinclude -Ivendor/eigen -I. -include nanospline/CurveNetwork.h -include nanospline/ReparameterizedBSpline.h src/CurveNetwork.cpp src/ReparameterizedBSpline.cpp examples/curve_network.cpp -o .task-build/example
rm -f .task-build/network.obj
.task-build/example .task-build/network.obj
test -s .task-build/network.obj
grep -q '^v ' .task-build/network.obj
grep -q '^vn ' .task-build/network.obj
grep -q '^f ' .task-build/network.obj
  output_log=$(mktemp 2>/dev/null || mktemp .task-build/tasklog.XXXXXX)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in synchronization surface compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
