#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/frame_sequence_model.c; fi
mkdir -p .task-build
cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. -Ilib -c lib/lz4.c -o .task-build/lz4.o
for f in lz4hc lz4frame xxhash; do cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. -Ilib -c lib/$f.c -o .task-build/$f.o; done
if [[ $mode == target ]]; then
  cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -I. -Ilib task_support/driver.c tests/frame_sequence_model.c .task-build/*.o -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in sequence integrity recovery; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
cc -std=c99 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ilib task_support/baseline.c .task-build/*.o -o .task-build/baseline
.task-build/baseline
