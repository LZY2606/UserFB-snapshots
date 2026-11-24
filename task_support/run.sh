#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/location_views_contract.c; fi
mkdir -p .task-build
cmake -S . -B .task-build -DENABLE_DECOMPRESSION=OFF -DBUILD_DWARFDUMP=ON -DBUILD_DWARFGEN=OFF -DBUILD_DWARFEXAMPLE=OFF -DDO_TESTING=OFF > .task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build --target dwarf-static dwarfdump -j 4 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
cc -std=c99 -I. -Isrc/lib/libdwarf task_support/baseline.c .task-build/src/lib/libdwarf/libdwarf.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Isrc/lib/libdwarf task_support/driver.c tests/location_views_contract.c .task-build/src/lib/libdwarf/libdwarf.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in inline_views external_views consumer_views; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
