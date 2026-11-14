#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s src/tests/mixed_seek.c; fi
cmake -S ports/cmake -B .task-build -DMACHINE=generic -DBUILD_PROGRAMS=OFF -DBUILD_LIBOUT123=OFF -DBUILD_SHARED_LIBS=OFF -DNETWORK=OFF -DCMAKE_BUILD_TYPE=Release > .task-configure.log 2>&1 || { tail -30 .task-configure.log; exit 1; }
cmake --build .task-build --target libmpg123 -j 2 > .task-build.log 2>&1 || { tail -30 .task-build.log; exit 1; }
cc -std=c99 -O2 -I. -Isrc/include task_support/baseline.c .task-build/src/libmpg123/libmpg123.a -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -I. -Isrc/include -I.task-build/src -Isrc/libmpg123 task_support/driver.c src/tests/mixed_seek.c .task-build/src/libmpg123/libmpg123.a -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in timeline seeking lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
