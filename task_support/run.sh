#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/script_replay_contract.c; fi
test "$mode" != target || test -s src/script_replay.c
rm -rf .task-build
mkdir .task-build
for source in src/*.c; do cc -std=c99 -O0 -g -I. -Isrc -c "$source" -o ".task-build/$(basename "$source" .c).o"; done
ar rcs .task-build/libgumbo.a .task-build/*.o
cc -std=c99 -O0 -I. -Isrc task_support/baseline.c .task-build/libgumbo.a -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O0 -g -I. -Isrc task_support/driver.c tests/script_replay_contract.c .task-build/libgumbo.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in INSERTION CONTEXT SOURCE; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
