#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/ray_spawn.c; fi
if [[ $mode == target ]]; then test -s src/lib/renderer/ray_spawn.c; test -s src/lib/renderer/ray_spawn.h; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -D_POSIX_C_SOURCE=200112L -I. -Iinclude -Isrc -Isrc/lib -Isrc/common task_support/driver.c task_tests/ray_spawn.c .task-build/libcray.a -lm -lpthread -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in parameter spawn transport; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O1 -D_POSIX_C_SOURCE=200112L -I. -Iinclude -Isrc -Isrc/lib -Isrc/common task_support/baseline.c .task-build/libcray.a -lm -lpthread -o .task-build/baseline
.task-build/baseline
