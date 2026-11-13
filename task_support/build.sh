#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build/task
cxx=${CXX:-c++}
for source in src/*.cpp; do
  obj="build/task/$(basename "${source%.cpp}").o"
  if [[ ! -f "$obj" || "$source" -nt "$obj" || -n "$(find src -name '*.h' -newer "$obj" -print -quit)" ]]; then
    "$cxx" -std=c++11 -O1 -g -Isrc -c "$source" -o "$obj"
  fi
done
ar rcs build/task/libmeshoptimizer.a build/task/*.o
"$cxx" -std=c++11 -O1 -g -Isrc demo/tests.cpp task_support/old_main.cpp build/task/libmeshoptimizer.a -o build/task/old_tests
"$cxx" -std=c++11 -O1 -g -Isrc task_support/baseline.cpp build/task/libmeshoptimizer.a -o build/task/baseline
