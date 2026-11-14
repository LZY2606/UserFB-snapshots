#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/nlsf_contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CC:-cc} -std=c99 -O2 -DHAVE_CONFIG_H -DOPUS_BUILD -DTASK_NLSF_TEST -I. -Iinclude -Isilk -Icelt -I.task-build task_support/driver.c tests/nlsf_contract.c .task-build/libopus.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in vectors history packets; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/legacy_projection
.task-build/test_opus_padding
echo BASELINE_PADDING_OK
