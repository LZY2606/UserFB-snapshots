#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
export CCACHE_DISABLE=1
if [ "${1:-}" = baseline ]; then
 cc -std=c99 -O1 -I. task_support/baseline.c renderer/core/*.c renderer/shaders/*.c -lm -o .task-build/baseline
 exec .task-build/baseline
fi
test -s task_tests/target.c
cc -std=c99 -O1 -I. -Itask_support task_support/runner.c task_tests/target.c renderer/core/*.c renderer/shaders/*.c -lm -o .task-build/target
cc -std=c99 -O1 -I. -Itask_support examples/projected_decals.c renderer/core/*.c renderer/shaders/*.c -lm -o .task-build/example
.task-build/example .task-build/demo.ppm
test -s .task-build/demo.ppm
.task-build/target
