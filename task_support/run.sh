#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/test_chunks.c; fi
bash task_support/build.sh baseline
${CC:-cc} -std=c99 -O2 -w -DCOMPILE_10BIT=0 -DUAVS3E_SCALAR_ONLY -Iinc -I. task_support/baseline.c .task-build/base-lib/libuavs3e.a -lm -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  bash task_support/build.sh target
${CC:-cc} -std=c99 -O2 -w -DCOMPILE_10BIT=0 -DUAVS3E_SCALAR_ONLY -DUAVS3E_TASK_TESTING=1 -Iinc -I. task_support/driver.c task_tests/test_chunks.c .task-build/target-lib/libuavs3e.a -lm -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in prefix coding lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
