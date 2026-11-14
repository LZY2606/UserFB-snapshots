#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/feature_cases.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I. -c our_gl.cpp -o .task-build/our_gl.o
c++ -std=c++17 -O2 -I. -c tgaimage.cpp -o .task-build/tgaimage.o
c++ -std=c++17 -O2 -I. -c model.cpp -o .task-build/model.o
c++ -std=c++17 -O2 -I. task_support/baseline.cpp .task-build/our_gl.o .task-build/tgaimage.o .task-build/model.o -o .task-build/baseline
c++ -std=c++17 -O2 -I. main.cpp .task-build/our_gl.o .task-build/tgaimage.o .task-build/model.o $(if [[ $mode == target ]]; then printf '%s' feature_edges.cpp; fi) -o .task-build/preview
if [[ $mode == target ]]; then
  test -s feature_edges.cpp
c++ -std=c++17 -O2 -I. tests/feature_cases.cpp feature_edges.cpp task_support/driver.c .task-build/our_gl.o .task-build/tgaimage.o .task-build/model.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in topology visibility integration; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
