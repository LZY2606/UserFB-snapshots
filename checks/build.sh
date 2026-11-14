#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p .task-build/objects
for input in lib/fuzzer/*.cpp; do
  object=".task-build/objects/$(basename "${input%.cpp}").o"
  if [[ ! -f "$object" || "$input" -nt "$object" ]] || find lib/fuzzer -maxdepth 1 -name '*.h' -newer "$object" -print -quit | grep -q .; then
    "${CXX:-clang++}" -std=c++11 -O1 -g -fno-omit-frame-pointer -c "$input" -o "$object"
  fi
done
ar rcs .task-build/libFuzzer.a .task-build/objects/*.o
