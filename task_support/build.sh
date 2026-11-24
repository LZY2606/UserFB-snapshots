#!/bin/bash
set -euo pipefail
mkdir -p .task-build
cxx=${CXX:-c++}
objects=()
for source in src/*.cpp; do
  object=".task-build/$(basename "${source%.cpp}").o"
  "$cxx" -std=c++17 -O0 -I. -Isrc -c "$source" -o "$object"
  objects+=("$object")
done
for source in src/Examples/ExamplePDBSize.cpp src/Examples/ExampleTimedScope.cpp; do
  object=".task-build/$(basename "${source%.cpp}").o"
  "$cxx" -std=c++17 -O0 -I. -Isrc -include src/PDB_PCH.h -c "$source" -o "$object"
  objects+=("$object")
done
rm -f .task-build/libraw_pdb.a
ar rcs .task-build/libraw_pdb.a "${objects[@]}"
"$cxx" -std=c++17 -O0 -I. -Isrc task_support/baseline.cpp .task-build/libraw_pdb.a -o .task-build/baseline
