#!/bin/bash
set -euo pipefail
mkdir -p .task-build
rm -f .task-build/libproduction.a
c++ -x c++ -c /dev/null -o .task-build/empty.o
objects=(.task-build/empty.o)
while IFS= read -r -d '' file; do
  object=".task-build/prod-${#objects[@]}.o"
  c++ -std=c++17 -O2 -pthread -I. -c "$file" -o "$object"
  objects+=("$object")
done < <(find task_src -type f -name '*.cpp' -print0 | sort -z)
ar rcs .task-build/libproduction.a "${objects[@]}"
c++ -std=c++17 -O2 -pthread -I. task_support/baseline.cpp .task-build/libproduction.a -o .task-build/baseline
