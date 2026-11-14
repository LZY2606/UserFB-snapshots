#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cxx=${CXX:-c++}
"$cxx" -std=c++20 -O1 -pthread -I. -Isrc task_support/baseline.cpp -o .task-build/baseline
object_index=0
: > .task-build/objects.rsp
while IFS= read -r -d '' source; do
  object=".task-build/production_$object_index.o"
  "$cxx" -std=c++20 -O1 -pthread -I. -Isrc -c "$source" -o "$object"
  printf '%s\n' "$object" >> .task-build/objects.rsp
  object_index=$((object_index+1))
done < <(find src/bvh/v2 -path '*/c_api' -prune -o -name '*.cpp' -type f -print0)
if [[ ${1:-baseline} == target ]]; then
  "$cxx" -std=c++20 -O1 -pthread -I. -Isrc checks/wide_cases.cpp task_support/driver.c @.task-build/objects.rsp -o .task-build/target
fi
