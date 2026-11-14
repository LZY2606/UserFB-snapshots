#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in ./*.cpp; do
  test -s "$source"
  object=".task-build/$(basename "${source%.cpp}").o"
  c++ -std=c++11 -I. -c "$source" -o "$object"
  objects+=("$object")
done
ar rcs .task-build/libhtmlparser.a "${objects[@]}"
c++ -std=c++11 -I. task_support/baseline.cpp .task-build/libhtmlparser.a -o .task-build/baseline
c++ -x c++ -std=c++11 -I. -c task_support/driver.c -o .task-build/driver.o
