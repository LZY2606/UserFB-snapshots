#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/live_message.cpp; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CXX:-c++} -std=c++11 -DENABLE_STDCXX_SYNC=1 -DENABLE_LOGGING=1 -I. -Isrtcore -Ihaicrypt -I.task-build/srt -x c++ task_support/driver.c task_tests/live_message.cpp -x none .task-build/srt/libsrt.a -pthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in message deadline api; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
