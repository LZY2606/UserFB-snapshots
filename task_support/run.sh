#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/channel_alignment.cc; fi
mkdir -p .task-build/objects
objects=()
for production in source/*.cc; do object=.task-build/objects/$(basename "${production%.cc}").o; c++ -std=c++11 -O2 -Isource -pthread -c "$production" -o "$object"; objects+=("$object"); done
rm -f .task-build/libzita.a
ar rcs .task-build/libzita.a "${objects[@]}"
c++ -std=c++11 -O2 -Isource task_support/baseline.cc .task-build/libzita.a -pthread -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  c++ -std=c++11 -O2 -I. -Isource -x c++ tests/channel_alignment.cc task_support/driver.c -x none .task-build/objects/*.o -pthread -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in coordinate window streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
