#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/embedded_qmf_contract.c; fi
mkdir -p .task-build
if [[ $mode == target ]]; then
  ${CC:-cc} -std=c99 -O2 -I. task_support/driver.c tests/embedded_qmf_contract.c g722_encode.c g722_decode.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in embedded analysis synthesis packing; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
sh checks/baseline.sh
