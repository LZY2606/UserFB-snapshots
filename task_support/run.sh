#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/adu_target.c; fi
cmake -S . -B .task-build -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build -j 4
if [[ $mode == target ]]; then
  cc -std=c99 -DHAVE_CONFIG_H -DFPM_64BIT -DOPT_ACCURACY -I. -I.task-build task_support/driver.c tests/adu_target.c .task-build/CMakeFiles/mad_task.dir/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in packetize decode lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
