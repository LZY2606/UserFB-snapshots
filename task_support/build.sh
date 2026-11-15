#!/bin/bash
set -euo pipefail
mkdir -p .task-build
if ! cmake -S . -B .task-build/lib -DLEXBOR_BUILD_STATIC=ON -DLEXBOR_BUILD_SHARED=OFF -DLEXBOR_BUILD_TESTS=OFF -DLEXBOR_BUILD_EXAMPLES=OFF -DLEXBOR_BUILD_UTILS=OFF -DLEXBOR_BUILD_BENCHMARKS=OFF >.task-build/configure.log 2>&1; then tail -30 .task-build/configure.log; exit 1; fi
if ! cmake --build .task-build/lib -j 4 >.task-build/build.log 2>&1; then tail -30 .task-build/build.log; exit 1; fi
cc -std=c99 -I. -Isource task_support/baseline.c .task-build/lib/liblexbor_static.a -lm -o .task-build/baseline
