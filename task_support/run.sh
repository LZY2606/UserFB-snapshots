#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
export CCACHE_DISABLE=1
export CLANG_MODULE_CACHE_PATH="$(pwd)/.task-build/modules"
mkdir -p "$CLANG_MODULE_CACHE_PATH"
if [ "${1:-}" = baseline ]; then
 c++ -std=c++17 -O1 -I. -Itask_support task_support/baseline.cpp  -o .task-build/baseline
 exec .task-build/baseline
fi
test -s task_tests/target.cpp
c++ -std=c++17 -O1 -I. -Itask_support task_support/runner.cpp task_tests/target.cpp  -o .task-build/target
c++ -std=c++17 -O1 -I. -Itask_support examples/opacity_layers.cpp  -o .task-build/example
.task-build/example .task-build/demo.ppm
test -s .task-build/demo.ppm
.task-build/target
