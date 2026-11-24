#!/usr/bin/env bash
set -euo pipefail
mkdir -p .task-build
task_sources=()
while IFS= read -r -d '' task_source; do
  task_sources+=("$task_source")
done < <(find src -type f -name '*.cpp' -print0)
c++ -std=c++17 -I. -Iinclude task_support/baseline.cpp ${task_sources[@]+"${task_sources[@]}"} \
  -o .task-build/baseline
