#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/aux_contract.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. -Isrc -c src/cmixer.c -o .task-build/cmixer.o
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Isrc task_support/driver.c tests/aux_contract.c .task-build/cmixer.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in routing tails lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O2 -I. -Isrc task_support/baseline.c .task-build/cmixer.o -lm -o .task-build/baseline
.task-build/baseline
