#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/task_compact.c; fi
bash task_support/build.sh
if [[ $mode == target ]]; then
  ${CC:-cc} -std=c11 -O0 -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I. -Iinclude -Idependencies/zycore/include task_support/driver.c tests/task_compact.c .task-build/cmake/libZydis.a .task-build/cmake/zycore/libZycore.a -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in MaskDag LaneRanks MemoryVersions Composition; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash task_support/legacy.sh
