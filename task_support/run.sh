#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/resource_mapping.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -Iinclude -I. -x c++ -c task_support/driver.c -o .task-build/check-driver.o
c++ -std=c++17 -Iinclude -I. tests/resource_mapping.cpp .task-build/check-driver.o .task-build/libpelib.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Mapping Aliases Reread; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
