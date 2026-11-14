#!/bin/bash
set -euo pipefail
mkdir -p .task-build
objects=()
for source in src/core/*.cpp src/backend/null/soloud_null.cpp; do
 object=.task-build/$(basename "${source%.cpp}").o
 objects+=("$object")
 if [[ ! -e $object || $source -nt $object ]] || find include -type f -newer "$object" -print -quit 2>/dev/null | grep -q .; then
  c++ -std=c++17 -O1 -DWITH_NULL -Iinclude -I. -pthread -c "$source" -o "$object"
 fi
done
rm -f .task-build/libsoloud.a
ar rcs .task-build/libsoloud.a "${objects[@]}"
