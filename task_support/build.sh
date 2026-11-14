#!/bin/bash
set -euo pipefail
mkdir -p .task-build
if ! cmake -S . -B .task-build/lib -DBUILD_SHARED_LIBS=OFF -DBUILD_COMMAND_LINE_TOOLS=OFF -DCMAKE_BUILD_TYPE=Debug > .task-build/configure.log 2>&1; then tail -25 .task-build/configure.log; exit 1; fi
if ! cmake --build .task-build/lib -j2 > .task-build/compile.log 2>&1; then tail -25 .task-build/compile.log; exit 1; fi
for name in legacy_import legacy_reloc; do
 c++ -std=c++11 -Ipe-parser-library/include task_support/$name.cpp .task-build/lib/pe-parser-library/libpe-parse.a -o .task-build/$name
done
