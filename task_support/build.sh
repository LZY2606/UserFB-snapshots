#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
# Rebuild on every invocation; never use an old regression binary.
objects=()
while IFS= read -r srcfile; do
  obj=".task-build/$(echo "$srcfile" | tr / _).o"
  c++ -std=c++14 -O1 -Wno-deprecated-declarations -Isrc -c "$srcfile" -o "$obj"
  objects+=("$obj")
done < task_support/core-sources.txt
ar rcs .task-build/core.a "${objects[@]}"
c++ -std=c++14 -O1 -Wno-deprecated-declarations -Isrc task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
c++ -std=c++14 -O1 -Wno-deprecated-declarations -Itask_support/legacy_gtest -Isrc task_support/legacy_main.cpp "${objects[@]}" -o .task-build/legacy
