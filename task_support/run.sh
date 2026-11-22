#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/precise_clock_test.c; fi
mkdir -p .task-build/obj
rm -f .task-build/obj/*.o .task-build/libspeex-resample.a
for unit in libspeexdsp/resample*.c; do
  cc -std=c99 -O2 -DOUTSIDE_SPEEX -DRANDOM_PREFIX=taskseed -DFLOATING_POINT -Iinclude/speex -Ilibspeexdsp -c "$unit" -o ".task-build/obj/$(basename "${unit%.c}").o"
done
ar rcs .task-build/libspeex-resample.a .task-build/obj/*.o
if [[ $mode == target ]]; then
  cc -std=c99 -O2 -DOUTSIDE_SPEEX -DRANDOM_PREFIX=taskseed -DFLOATING_POINT -Iinclude/speex -Ilibspeexdsp -I. tests/precise_clock_test.c task_support/driver.c .task-build/libspeex-resample.a -lm -o .task-build/target
  output_log=.task-build/target-output.log
  : >"$output_log"
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in arithmetic audio streaming; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then
  cc -std=c99 -O2 -DOUTSIDE_SPEEX -DRANDOM_PREFIX=taskseed -DFLOATING_POINT -Iinclude/speex -Ilibspeexdsp task_support/baseline.c .task-build/libspeex-resample.a -lm -o .task-build/baseline
  .task-build/baseline
fi
