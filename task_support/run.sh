#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/envelope/contract.c; fi
mkdir -p .task-build
task_cc=${CC:-cc}
task_cflags='-std=c99 -O0 -DMA_NO_DEVICE_IO -DMA_NO_RESOURCE_MANAGER -DMA_NO_DECODING -DMA_NO_ENCODING -I.'
$task_cc $task_cflags -c miniaudio.c -o .task-build/miniaudio.o
$task_cc $task_cflags task_support/baseline.c .task-build/miniaudio.o -lm -pthread -o .task-build/baseline
if [[ $mode == target ]]; then
  $task_cc $task_cflags tests/envelope/contract.c task_support/driver.c .task-build/miniaudio.o -lm -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in curve source_clock render_paths; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
