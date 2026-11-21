#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/test_seek.cpp; fi
mkdir -p .task-build
${CXX:-c++} -std=c++17 -O2 -pthread -I. task_support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  extra_sources=(); if [[ -d task_src ]]; then while IFS= read -r -d "" source; do extra_sources+=("$source"); done < <(find task_src -type f -name "*.cpp" -print0); fi
${CXX:-c++} -std=c++17 -O2 -pthread -I. task_support/driver.c task_tests/test_seek.cpp ${extra_sources[@]+"${extra_sources[@]}"} -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in suffix phase bounded; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
