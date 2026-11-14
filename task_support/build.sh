#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
objects=()
for src in src/pelib/*.cpp; do
 obj=.task-build/obj/$(basename "${src%.cpp}").o
 if [[ ! -f $obj || $src -nt $obj ]] || find include -type f -newer "$obj" -print -quit 2>/dev/null | grep -q .; then
  c++ -std=c++17 -O0 -Iinclude -I. -c "$src" -o "$obj"
 fi
 objects+=("$obj")
done
rm -f .task-build/libpelib.a
ar rcs .task-build/libpelib.a "${objects[@]}"
c++ -std=c++17 -O0 -Iinclude -I. task_support/baseline.cpp .task-build/libpelib.a -o .task-build/baseline
