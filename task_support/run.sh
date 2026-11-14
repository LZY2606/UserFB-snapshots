#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/field_prediction_cases.c; fi
mkdir -p .task-build
printf '#define HAVE_BUILTIN_EXPECT 1\n#define ATTRIBUTE_ALIGNED_MAX 16\n' > .task-build/config.h
for src in libmpeg2/{alloc,header,decode,slice,motion_comp,idct,cpu_accel,cpu_state}.c; do cc -O2 -std=c99 -I.task-build -Iinclude -Ilibmpeg2 -c "$src" -o ".task-build/$(basename "$src" .c).o"; done
if [[ $mode == target ]]; then
  cc -O2 -std=c99 -I. -I.task-build -Iinclude -Ilibmpeg2 task_support/driver.c tests/field_prediction_cases.c .task-build/*.o -lm -o .task-build/target
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in frame_dual field_pair predictors mutations; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
bash support/baseline.sh
