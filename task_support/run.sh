#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p .task-build
export CCACHE_DISABLE=1
export CLANG_MODULE_CACHE_PATH="$(pwd)/.task-build/modules"
mkdir -p "$CLANG_MODULE_CACHE_PATH"
if [ "${1:-}" = baseline ]; then
 c++ -std=c++20 -O1 -I. -Itask_support task_support/baseline.cpp our_gl.cpp tgaimage.cpp -o .task-build/baseline
 exec .task-build/baseline
fi
test -s task_tests/target.cpp
c++ -std=c++20 -O1 -I. -Itask_support task_support/runner.cpp task_tests/target.cpp transparency.cpp our_gl.cpp tgaimage.cpp -o .task-build/target
c++ -std=c++20 -O1 -I. -Itask_support examples/transparency.cpp transparency.cpp our_gl.cpp tgaimage.cpp -o .task-build/example
.task-build/example .task-build/demo.tga
test -s .task-build/demo.tga
.task-build/target
