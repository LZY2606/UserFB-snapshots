#!/bin/bash
set -euo pipefail
mkdir -p .task-build
# Use a nonempty archive list; every added core translation unit is compiled.
for source_file in src/core/*.cpp src/backend/null/soloud_null.cpp; do
  test -s "$source_file"
  object_file=".task-build/$(basename "$source_file" .cpp).o"
  if [[ ! -f "$object_file" || "$source_file" -nt "$object_file" ]] || find include -type f -newer "$object_file" -print -quit | grep -q .; then
    c++ -std=c++17 -O1 -DWITH_NULL -Iinclude -c "$source_file" -o "$object_file" -pthread
  fi
done
rm -f .task-build/libsoloud.a
ar rcs .task-build/libsoloud.a .task-build/soloud*.o
