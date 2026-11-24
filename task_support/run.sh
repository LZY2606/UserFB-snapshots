#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/h264_access_units.cpp; fi
if [[ $mode == target ]]; then
  test -s mpegts/h264_access_unit.cpp
  test -s mpegts/h264_access_unit.h
fi
mkdir -p .task-build
rm -f .task-build/*.o
for source in mpegts/*.cpp; do
  c++ -std=c++17 -O1 -pthread -include unordered_map -I. -Impegts -c "$source" -o ".task-build/$(basename "$source" .cpp).o"
done
if [[ $mode == target ]]; then
  c++ -std=c++17 -O1 -pthread -include unordered_map -I. -Impegts -x c++ tests/h264_access_units.cpp task_support/driver.c -x none .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in pictures timestamps lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
c++ -std=c++17 -O1 -pthread -include unordered_map -I. -Impegts task_support/baseline.cpp .task-build/*.o -o .task-build/baseline
.task-build/baseline
