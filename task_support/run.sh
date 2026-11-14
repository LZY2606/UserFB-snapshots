#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/policy_contract.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CC:-cc} -std=c99 -O0 -I. -Itask_support -Isrc -Isrc/common task_support/driver.c test/policy_contract.c .task-build/fd.o .task-build/fuzz.o .task-build/random.o .task-build/ranges.o -lm -o .task-build/target
  output_log=$(mktemp "${TMPDIR:-/tmp}/zzuf-target.XXXXXX")
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in mask_model policy_composition context_lifecycle pushback_schedule; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
.task-build/zzuf-stdin -s1812 -r.013 < .task-build/baseline-input.bin > .task-build/baseline-output.bin
cmp .task-build/baseline-expect.bin .task-build/baseline-output.bin
echo ZZUF_STDIN_BASELINE_OK
