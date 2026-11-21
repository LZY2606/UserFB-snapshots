#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/pose_graph.c; fi
mkdir -p .task-build
find src -name '*.c' -type f | LC_ALL=C sort > .task-build/sources
test -s .task-build/sources
while IFS= read -r f; do cc -std=c99 -O1 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Isrc -I. -c "$f" -o ".task-build/$(basename "$f" .c).o"; done < .task-build/sources
cc -std=c99 -O1 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Isrc -I. task_support/baseline.c .task-build/*.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/pose_graph.h
test -s src/pose_graph.c
test -s example/pose-graph-cpu.c
cc -std=c99 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Isrc -I. -include src/pose_graph.h -c src/pose_graph.c -o .task-build/pose_graph.o
cc -std=c99 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Isrc -I. tests/pose_graph.c task_support/driver.c .task-build/*.o -lm -o .task-build/target
cc -std=c99 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -Isrc -I. example/pose-graph-cpu.c .task-build/*.o -lm -o .task-build/demo
.task-build/demo
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in objective publication lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
