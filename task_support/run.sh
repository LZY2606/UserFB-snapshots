#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s test/pending_pcm.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -DVORBIS_PENDING_TRACE -I. -Iinclude -Ilib -Itask_support/include -Itask_support/vendor/ogg/include task_support/driver.c test/pending_pcm.c .task-build/libcodec.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in Queue Timeline Compatibility; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
