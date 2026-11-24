#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/forward_origins.cc; fi
mkdir -p .task-build
objects=()
for source in src/common/*.cc src/common/dwarf/*.cc; do
  object=.task-build/$(basename "$source" .cc).o
  c++ -std=c++17 -O0 -I. -Isrc -c "$source" -o "$object"
  objects+=("$object")
done
c++ -std=c++17 -O0 -I. -Isrc task_support/baseline.cc "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++17 -O0 -I. -Isrc task_support/driver.c tests/forward_origins.cc "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fanout convergence lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
