#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sources=(libs/fssr/basis_function.cc libs/fssr/hermite.cc libs/fssr/iso_octree.cc libs/fssr/iso_surface.cc libs/fssr/octree.cc libs/fssr/triangulation.cc libs/fssr/voxel.cc libs/mve/mesh.cc)
for src in "${sources[@]}"; do
  obj=.task-build/$(basename "$src").o
    c++ -std=c++17 -O1 -Ilibs -c "$src" -o "$obj"
done
c++ -std=c++17 -O1 -Ilibs task_support/baseline.cpp .task-build/*.cc.o -o .task-build/baseline
