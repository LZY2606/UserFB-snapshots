#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/compressed_contract.c; fi
make -f task_support/Makefile -j 4
if [[ $mode == target ]]; then
  cc -std=c11 -D_DEFAULT_SOURCE -I. -Ilibelf -Icommon -Itask_support/include -Itask_support/vendor/zlib task_support/driver.c tests/compressed_contract.c task_support/fixture.c .task-build/libelf.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in views symbols failures; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
