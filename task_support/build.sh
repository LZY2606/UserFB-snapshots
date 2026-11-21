#!/bin/bash
set -euo pipefail
flag=OFF
if [[ $1 == target ]]; then
 test -s src/wavl.h
 test -s src/wavl.c
 test -s examples/wavl_ima.c
 flag=ON
fi
mkdir -p .task-build
cmake -S task_support -B .task-build/production -DCMAKE_BUILD_TYPE=Release -DTASK_TARGET="$flag" >.task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/production --target baseline -j 2 >.task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
