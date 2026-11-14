#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build/srt -DCMAKE_BUILD_TYPE=Release -DENABLE_ENCRYPTION=OFF -DENABLE_APPS=OFF -DENABLE_TESTING=OFF -DENABLE_UNITTESTS=OFF -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DENABLE_BONDING=OFF -DENABLE_STDCXX_SYNC=ON -DENABLE_LOGGING=ON > .task-build/configure.log 2>&1 || { tail -30 .task-build/configure.log; exit 1; }
cmake --build .task-build/srt --target srt_static --parallel 4 > .task-build/build.log 2>&1 || { tail -30 .task-build/build.log; exit 1; }
"${CXX:-c++}" -std=c++11 -DENABLE_STDCXX_SYNC=1 -DENABLE_LOGGING=1 -I. -Isrtcore -Ihaicrypt -I.task-build/srt task_support/baseline.cpp .task-build/srt/libsrt.a -pthread -o .task-build/baseline
