#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/recovery.c; fi
mkdir -p .task-build
for source_file in Decoder/src/h264bsd_*.c Decoder/src/H264SwDecApi.c; do
  [[ $source_file == Decoder/src/h264bsd_sei.c ]] && continue
  object_name=${source_file##*/}
  cc -std=c99 -O1 -I. -IDecoder/inc -IDecoder/src -c "$source_file" -o ".task-build/${object_name%.c}.o"
done
cc -std=c99 -O1 -I. -IDecoder/inc -IDecoder/src task_support/baseline.c .task-build/*.o -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -I. -IDecoder/inc -IDecoder/src task_support/driver.c tests/recovery.c .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sei window output; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
