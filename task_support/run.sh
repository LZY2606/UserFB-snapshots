#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/surface_partition.cpp; fi
if [[ $mode == target ]]; then
  test -s include/manifold/surface_partition.h
  test -s src/surface_partition.cpp
  test -s examples/surface_partition.cpp
fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ar -t .task-build/cmake/src/libmanifold.a > .task-build/library-objects.txt
  grep -Eq '(^|/)surface_partition.cpp.o$' .task-build/library-objects.txt
fi
if [[ $mode == target ]]; then
  c++ -std=c++17 -O1 -I. -Iinclude -I.task-build/cmake/include -include include/manifold/surface_partition.h test/surface_partition.cpp task_support/driver.c .task-build/cmake/src/libmanifold.a -o .task-build/target
c++ -std=c++17 -O1 -I. -Iinclude -I.task-build/cmake/include -include include/manifold/surface_partition.h examples/surface_partition.cpp .task-build/cmake/src/libmanifold.a -o .task-build/example
rm -f .task-build/partition.obj
.task-build/example .task-build/partition.obj
test -s .task-build/partition.obj
  if ! output_log=$(mktemp 2>/dev/null); then
    # Some restricted sandboxes reject the platform user-temp location; the
    # build directory is already writable in that case.
    output_log=".task-build/target-output.$$.log"
  fi
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in junctions regions properties reuse; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
