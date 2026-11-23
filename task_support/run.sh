#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/clock_bridge.c; fi
mkdir -p .task-build
rm -f .task-build/prod-*.o
objects=()
for src in resampler.c task_src/*.c; do
  [[ -f "$src" ]] || continue
  obj=".task-build/prod-${#objects[@]}.o"
  cc -std=c99 -O2 -DPATH_WIDTH=64 -I. -Itask_src -c "$src" -o "$obj"
  objects+=("$obj")
done
cc -std=c99 -O2 -DPATH_WIDTH=64 -I. probes/baseline.c "${objects[@]}" -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -DPATH_WIDTH=64 -I. -Itask_src task_support/driver.c task_tests/clock_bridge.c "${objects[@]}" -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in clock audio streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
