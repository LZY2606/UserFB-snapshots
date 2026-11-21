#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build/library -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build .task-build/library --target faad -j 2
cc -std=c99 -O2 -Iinclude task_support/baseline.c .task-build/library/libfaad.a -lm -o .task-build/baseline
