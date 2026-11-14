#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s testing/quadratic_target.cpp; fi
mkdir -p .task-build/include
bash checks/assemble.sh .task-build/include/portablegl.h
c++ -std=c++17 -O2 -DPGL_CHUNK_PRIMS=64 -I.task-build/include -I. -c checks/pgl_impl.cpp -o .task-build/pgl.o
cc -std=c99 -O1 -DPGL_CHUNK_PRIMS=64 -I.task-build/include -I. -x c -c checks/pgl_impl.cpp -o .task-build/pgl-c.o
c++ -std=c++17 -O2 -I.task-build/include -I. checks/baseline.cpp .task-build/pgl.o -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -DPGL_CHUNK_PRIMS=64 -I.task-build/include -I. task_support/driver.c testing/quadratic_target.cpp .task-build/pgl.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry attributes submission; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
