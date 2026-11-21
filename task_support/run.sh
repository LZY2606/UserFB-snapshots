#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/crc_recovery.c; fi
bash devcheck/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -DMINIMP3_CHECK_CRC -I. task_support/driver.c tests/crc_recovery.c .task-build/prod-*.o .task-build/reference.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in protected recovery extended; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash devcheck/run_baseline.sh
