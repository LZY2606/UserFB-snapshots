#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s contract/IdIndexRegression.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -O0 -DMyCORE_BUILD_WITHOUT_THREADS -Isource -I. task_support/driver.c contract/IdIndexRegression.c .task-build/libmyhtml.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in BUCKETS MUTATIONS LIFETIME; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
