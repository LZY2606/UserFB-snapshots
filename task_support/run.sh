#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then
  for required_production in include/nestegg/nestegg-events.h; do
    if [[ ! -s "$required_production" ]]; then
      echo "REQUIRED_PRODUCTION_MISSING_OR_EMPTY $required_production" >&2
      exit 1
    fi
  done
fi
if [[ $mode == target ]]; then test -s test/packet-events.c; fi
mkdir -p .task-build
objects=()
for f in src/*.c; do
  obj=".task-build/$(basename "$f" .c).o"
  cc -std=c99 -O2 -Iinclude -I. -c "$f" -o "$obj"
  objects+=("$obj")
done
cc -std=c99 -O2 -Iinclude -I. task_support/baseline.c "${objects[@]}" -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -Iinclude -I. task_support/driver.c test/packet-events.c "${objects[@]}" -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in fragments metadata seek_memory; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
