#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DMANIFOLD_TEST=OFF -DMANIFOLD_PAR=OFF -DMANIFOLD_CBIND=OFF -DMANIFOLD_PYBIND=OFF -DMANIFOLD_DOWNLOADS=OFF
rm -f .task-build/cmake/src/libmanifold.a
cmake --build .task-build/cmake --target manifold -j 2
c++ -std=c++17 -O1 -Iinclude -I.task-build/cmake/include task_support/baseline.cpp .task-build/cmake/src/libmanifold.a -o .task-build/baseline
