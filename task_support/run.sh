#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/property_copy_target.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Ic -Ivendor/libxml2/include -I.task-build/xml task_support/driver.c tests/property_copy_target.c .task-build/librdfa.a .task-build/xml/libxml2.a -lm -lpthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in copy_closure copy_terms copy_lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -Ic -Ivendor/libxml2/include -I.task-build/xml task_support/baseline.c .task-build/librdfa.a .task-build/xml/libxml2.a -lm -lpthread -o .task-build/baseline
.task-build/baseline
