#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_latency.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s src/core/soloud_latency.cpp
test -s include/soloud_latency.h
test -s examples/latency_render.cpp
c++ -std=c++17 -O1 -Iinclude -I. -include include/soloud_latency.h -fsyntax-only -x c++ /dev/null
c++ -std=c++17 -O1 -Iinclude -I. tests/task_latency.cpp task_support/driver.c .task-build/soloud*.o -pthread -o .task-build/target
c++ -std=c++17 -O1 -Iinclude -I. examples/latency_render.cpp .task-build/soloud*.o -pthread -o .task-build/latency_render
rm -f .task-build/latency_render.pcm
.task-build/latency_render .task-build/latency_render.pcm
test -s .task-build/latency_render.pcm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in graph pcm blocks; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++17 -O1 -Iinclude -I. task_support/baseline.cpp .task-build/libsoloud.a -pthread -o .task-build/baseline
.task-build/baseline
