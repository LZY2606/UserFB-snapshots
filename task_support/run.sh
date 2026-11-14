#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/ps_audio_mix.c; fi
mkdir -p .task-build
cc -std=c99 -O2 -I. task_support/baseline.c -lm -o .task-build/baseline
cc -std=c99 -O2 -I. task_support/ps_baseline.c -lm -o .task-build/ps-baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. task_support/driver.c tests/ps_audio_mix.c -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline mixing streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline task_support/media/mono.mp2 44100 1
.task-build/baseline task_support/media/stereo.mp2 48000 0
.task-build/ps-baseline
