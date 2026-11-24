#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s checks/incremental_verify.c; fi
mkdir -p .task-build
if [[ ! -f Makefile ]]; then bash checks/install.sh; else make -j2 libdmalloc.a; fi
if [[ $mode == target ]]; then
  cc -std=c99 -I. task_support/driver.c checks/incremental_verify.c libdmalloc.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in budget invalidation interleaving; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -I. checks/native_baseline.c libdmalloc.a -o .task-build/baseline
.task-build/baseline
