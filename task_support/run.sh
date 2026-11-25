#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/native/test_encode_inter_task.cpp; fi
mkdir -p .task-build
production_sources=$(find src -name '*.cpp' -print)
c++ -std=c++11 -O2 -I. -Isrc test/native/test_encode_iframe.cpp $production_sources -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -O2 -I. -Isrc task_support/driver.c test/native/test_encode_inter_task.cpp $production_sources -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in syntax reference sequence; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
(cd test/native && ../../.task-build/baseline)
