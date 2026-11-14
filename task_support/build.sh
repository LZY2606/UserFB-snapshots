#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in src/*.cpp; do
 [[ $source == src/Main.cpp ]] && continue
 object=".task-build/$(basename "${source%.cpp}").o"
 c++ -std=c++11 -O0 -I. -Isrc -c "$source" -o "$object"
 objects+=("$object")
done
c++ -std=c++11 -O0 -I. -Isrc task_support/baseline.cpp "${objects[@]}" -o .task-build/baseline
