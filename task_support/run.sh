#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/residual_reference.c; fi
bash repro/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -UNDEBUG -I. -Iinclude -Ilib -Ithird_party/ogg/include task_support/driver.c tests/residual_reference.c .task-build/libcodec.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in dc_neighbors stripe_state token_runs coefficients packet_pipeline; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash repro/baseline.sh
