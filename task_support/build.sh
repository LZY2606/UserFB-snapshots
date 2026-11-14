#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cxx=${CXX:-c++}
object_index=0
: > .task-build/objects.rsp
while IFS= read -r -d '' source; do
  object=".task-build/production_$object_index.o"
  "$cxx" -std=c++20 -O1 -I. -Isrc -c "$source" -o "$object"
  printf '%s\n' "$object" >> .task-build/objects.rsp
  object_index=$((object_index+1))
done < <(find src -name '*.cpp' -type f ! -path 'src/MiniLight.cpp' -print0)
"$cxx" -std=c++20 -O1 -I. -Isrc task_support/baseline.cpp @.task-build/objects.rsp -o .task-build/baseline
if [[ ${1:-baseline} == target ]]; then
  "$cxx" -std=c++20 -O1 -I. -Isrc checks/adaptive_cases.cpp task_support/driver.c @.task-build/objects.rsp -o .task-build/target
fi
