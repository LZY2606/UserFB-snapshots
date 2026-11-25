#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_prefix_index.c; fi
mkdir -p .task-build/lib
(cd .task-build/lib && ../../configure --cc=cc > ../build.log 2>&1 && make -j 4 lib >> ../build.log 2>&1)
if [[ $mode == target ]]; then
  cc -std=c99 -I. task_support/driver.c tests/task_prefix_index.c .task-build/lib/liblsmash.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in layout offsets lifecycle compat; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  cc -std=c99 -I. task_support/baseline.c .task-build/lib/liblsmash.a -lm -o .task-build/baseline
  .task-build/baseline
fi
