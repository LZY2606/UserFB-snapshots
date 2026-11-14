#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in renderer.h isosurface.h isosurface.cpp; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s tests/isosurface_target.cpp; fi
mkdir -p .task-build
c++ -std=c++17 -O2 -I. support/baseline.cpp -o .task-build/baseline
if [[ $mode == target ]]; then
  production_units=()
for unit in ./*.cpp; do [[ -f "$unit" ]] && production_units+=("$unit"); done
c++ -std=c++17 -O2 -DTINYRT_NO_MAIN -I. task_support/driver.c tests/isosurface_target.cpp "${production_units[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in roots traversal render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
