#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/forward_io_target.cpp; fi
source task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -I. -Iinclude -x c++ task_support/driver.c -x none tests/forward_io_target.cpp ${task_sources[@]+"${task_sources[@]}"} -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in reader_forward writer_forward pipe_roundtrip; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
