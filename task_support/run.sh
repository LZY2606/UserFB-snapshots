#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_support/transition_cases.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  test -s task_support/transition_oracle.c
cc -std=c17 -O2 -Wall -Wextra task_support/transition_oracle.c -o .task-build/transition_oracle -lm
cc -std=c17 -O2 -Wall -Wextra -I. task_support/driver.c task_support/transition_cases.c -o .task-build/target -lm
export FFMPEG_BIN="${FFMPEG_BIN:-$PWD/.task-build/ffmpeg/ffmpeg}"
export FFPROBE_BIN="${FFPROBE_BIN:-$PWD/.task-build/ffmpeg/ffprobe}"
export TRANSITION_ORACLE_BIN="$PWD/.task-build/transition_oracle"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in rational_timeline chunk_invariance oracle_guards; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
