#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
objects=()
while IFS= read -r srcfile; do
  obj=".task-build/${srcfile//\//_}.o"
  c++ -std=c++14 -O1 -Wno-deprecated-declarations -Isrc -c "$srcfile" -o "$obj"
  objects+=("$obj")
done < task_support/core-sources.txt
ar rcs .task-build/core.a "${objects[@]}"
c++ -std=c++14 -O1 -Wno-deprecated-declarations -Isrc task_support/baseline.cpp .task-build/core.a -o .task-build/baseline
c++ -std=c++14 -O1 -Wno-deprecated-declarations -Itask_support/legacy_gtest -Isrc task_support/legacy_main.cpp .task-build/core.a -o .task-build/legacy
