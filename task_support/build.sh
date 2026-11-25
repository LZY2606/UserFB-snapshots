#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/libproduction.a
objects=()
while IFS= read -r -d '' file; do
  object=".task-build/prod-${#objects[@]}.o"
  c++ -std=c++17 -O2 -I. -c "$file" -o "$object"
  objects+=("$object")
done < <(find . -maxdepth 1 -type f -name '*.cpp' -print0; find task_src -type f -name '*.cpp' -print0)
test "${#objects[@]}" -ge 4
ar rcs .task-build/libproduction.a "${objects[@]}"
c++ -std=c++17 -O2 -I. task_support/baseline.cpp .task-build/libproduction.a -o .task-build/baseline
