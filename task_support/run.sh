#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/probe.c; fi
mkdir -p .task-build
cc -std=c99 -O1 -Iinclude -I. -c clip.c -o .task-build/clip.o
cc -std=c99 -O1 -Iinclude -I. -c gl.c -o .task-build/gl.o
cc -std=c99 -O1 -Iinclude -I. -c map.c -o .task-build/map.o
cc -std=c99 -O1 -Iinclude -I. -c mat.c -o .task-build/mat.o
cc -std=c99 -O1 -Iinclude -I. -c obj.c -o .task-build/obj.o
cc -std=c99 -O1 -Iinclude -I. -c pipe.c -o .task-build/pipe.o
cc -std=c99 -O1 -Iinclude -I. -c rast.c -o .task-build/rast.o
cc -std=c99 -O1 -Iinclude -I. -c tga.c -o .task-build/tga.o
cc -std=c99 -I. -Itask_support -c task_support/driver.c -o .task-build/driver.o
target_objects=()
if [[ $mode == target ]]; then
  test -s include/probe.h
  test -s probe.c
  cc -std=c99 -O1 -Iinclude -I. -include include/probe.h -c probe.c -o .task-build/probe.o
  target_objects=(.task-build/probe.o)
fi
cc -std=c99 -Iinclude -I. task_support/baseline.c .task-build/clip.o .task-build/gl.o .task-build/map.o .task-build/mat.o .task-build/obj.o .task-build/pipe.o .task-build/rast.o .task-build/tga.o ${target_objects[@]+"${target_objects[@]}"} -lm -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s include/probe.h
test -s probe.c
test -s examples/probe.c
cc -std=c99 -O1 -Iinclude -I. -include include/probe.h -c probe.c -o .task-build/probe.o
cc -std=c99 -O1 -Iinclude -I. task_tests/probe.c .task-build/driver.o .task-build/probe.o .task-build/clip.o .task-build/gl.o .task-build/map.o .task-build/mat.o .task-build/obj.o .task-build/pipe.o .task-build/rast.o .task-build/tga.o -lm -o .task-build/target
cc -std=c99 -O1 -Iinclude -I. examples/probe.c .task-build/probe.o .task-build/clip.o .task-build/gl.o .task-build/map.o .task-build/mat.o .task-build/obj.o .task-build/pipe.o .task-build/rast.o .task-build/tga.o -lm -o .task-build/example
rm -rf .task-build/probe-output
mkdir -p .task-build/probe-output
.task-build/example .task-build/probe-output
test -s .task-build/probe-output/unbound.ppm
test -s .task-build/probe-output/partial.ppm
test -s .task-build/probe-output/complete.ppm
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in capture visibility publication; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
