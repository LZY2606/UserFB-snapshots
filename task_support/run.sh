#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/wsola_reference.cpp; fi
mkdir -p .task-build
bash task_support/build.sh
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -DSOUNDTOUCH_DISABLE_X86_OPTIMIZATIONS -I. -Iinclude -Isource/SoundTouch -I.build -x c++ task_support/driver.c tests/wsola_reference.cpp -x none .build/libsoundtouch.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in search overlap stream; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/baseline.sh
