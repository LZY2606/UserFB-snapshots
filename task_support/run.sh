#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/codebook_contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  "${CC:-cc}" -std=c99 -O2 -fwrapv -fno-strict-aliasing -I. -Iilbc/interface -Isignal_processing/include -I.task-build task_support/driver.c tests/codebook_contract.c .task-build/libilbc.a -pthread -lm -o .task-build/target
printf "ILBC_TARGET_LINK_OK\n"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in vectors residual stream; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
