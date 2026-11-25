#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/csg.cpp; fi
mkdir -p .task-build
find src -name '*.cpp' -type f | LC_ALL=C sort > .task-build/sources
test -s .task-build/sources
while IFS= read -r f; do c++ -std=c++17 -O1 -Itask_support/config -Isrc -I. -c "$f" -o ".task-build/$(basename "$f" .cpp).o"; done < .task-build/sources
c++ -std=c++17 -O1 -Itask_support/config -Isrc -I. tests/perspective_depth.cpp .task-build/*.o -o .task-build/depth_baseline
c++ -std=c++17 -O1 -Itask_support/config -Isrc -I. tests/mesh_instances.cpp .task-build/*.o -o .task-build/instances_baseline
if [[ $mode == target ]]; then
  test -s src/Csg.hpp
test -s src/Csg.cpp
test -s examples/csg.cpp
c++ -std=c++17 -O1 -Itask_support/config -Isrc -I. -include src/Csg.hpp -c src/Csg.cpp -o .task-build/Csg.o
c++ -std=c++17 -O1 -Itask_support/config -Isrc -I. task_tests/csg.cpp task_support/driver.c .task-build/*.o -o .task-build/target
c++ -std=c++17 -O1 -Itask_support/config -Isrc -I. examples/csg.cpp .task-build/*.o -o .task-build/demo
.task-build/demo
  output_log=".task-build/target-output.$$.log"
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in occupancy boundary lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/depth_baseline
.task-build/instances_baseline
printf '%s\n' JET_BASELINE_OK
