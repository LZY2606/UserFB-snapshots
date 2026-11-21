#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/stream_surface.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s libs/fssr/stream_surface.h
test -s libs/fssr/stream_surface.cc
test -s examples/stream_surface.cpp
c++ -std=c++17 -O1 -I. -Ilibs -include libs/fssr/stream_surface.h -c libs/fssr/stream_surface.cc -o .task-build/stream.o
c++ -std=c++17 -O1 -I. -Ilibs -include libs/fssr/stream_surface.h tests/stream_surface.cpp task_support/driver.c .task-build/*.cc.o .task-build/stream.o -o .task-build/target
c++ -std=c++17 -O1 -I. -Ilibs -include libs/fssr/stream_surface.h examples/stream_surface.cpp .task-build/*.cc.o .task-build/stream.o -o .task-build/example
mkdir -p .task-build/stream-tmp
rm -f .task-build/example.ply
.task-build/example .task-build/example.ply .task-build/stream-tmp
test -s .task-build/example.ply
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry budget publication compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
