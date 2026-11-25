#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in duma_access.c; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s test/access_contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CC:-cc} -std=gnu99 -w -pthread -DDUMA_NO_CPP_SUPPORT -DDUMA_NO_GLOBAL_MALLOC_FREE -DDUMA_EXPLICIT_INIT -DDUMA_PREFER_GETENV -DDUMA_NO_HANG_MSG -I. task_support/driver.c test/access_contract.c .task-build/*.o -o .task-build/target
  output_log=".task-build/target-output.$$.log"
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in attribution lifecycle ownership; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/old_healthy
