#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cmake -S . -B .task-build/cmake -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=ON -DZYDIS_BUILD_TESTS=OFF -DZYDIS_FEATURE_ENCODER=OFF -DZYCORE_BUILD_TESTS=OFF -DZYCORE_BUILD_EXAMPLES=OFF
cmake --build .task-build/cmake --parallel 2
${CC:-cc} -std=c11 -O0 -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -Iinclude -Idependencies/zycore/include task_support/legacy.c .task-build/cmake/libZydis.a .task-build/cmake/zycore/libZycore.a -o .task-build/legacy
