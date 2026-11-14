#!/bin/bash
set -euo pipefail
mkdir -p .task-build/obj
find src/library -type f \( -name '*.h' -o -name '*.cpp' \) -exec cksum {} + | LC_ALL=C sort > .task-build/signature.new
if ! cmp -s .task-build/signature.new .task-build/signature; then
 rm -f .task-build/obj/*.o .task-build/core.a
 # The editor and raytracer modules; no application, voxel importer or C ABI wrapper.
 find src/library -maxdepth 1 -name '*.cpp' ! -name voxelization.cpp ! -name cubiquity.cpp > .task-build/sources
 while IFS= read -r src; do
  obj=".task-build/obj/${src//\//_}.o"
  c++ -std=c++20 -O1 -Isrc/library -I. -c "$src" -o "$obj"
 done < .task-build/sources
 ar rcs .task-build/core.a .task-build/obj/*.o
 mv .task-build/signature.new .task-build/signature
fi
c++ -std=c++20 -O1 -Isrc/library -I. task_support/baseline.cpp .task-build/core.a -o .task-build/baseline
