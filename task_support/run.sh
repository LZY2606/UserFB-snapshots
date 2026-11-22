#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/format_barrier.c; fi
mkdir -p .task-build
rm -f .task-build/*.o
for source_file in *.c src/*.c; do
  test -f "$source_file" || continue
  test "$source_file" != main.c || continue
  object_name=$(echo "$source_file" | tr '/.' '__')
  cc -std=c11 -O2 -I. -Isrc -c "$source_file" -o ".task-build/$object_name.o"
done
cc -std=c11 -O2 -I. -Itask_support task_support/baseline.c .task-build/*.o -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  cc -std=c11 -O2 -I. -Isrc -Itask_support task_support/driver.c tests/format_barrier.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in events pcm lifecycle; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
