#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/sustain_crossfade.c; fi
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DBUILD_SHARED_LIBS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_ALLEGRO4=OFF -DUSE_SSE=OFF >.task-build/configure.log 2>&1
cmake --build .task-build/cmake --target dumb -j2 >.task-build/build.log 2>&1
cc -std=c99 -I. -Iinclude task_support/baseline.c .task-build/cmake/libdumb.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -I. -Iinclude tests/sustain_crossfade.c task_support/driver.c .task-build/cmake/libdumb.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in source release voices; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
