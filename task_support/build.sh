#!/bin/bash
set -euo pipefail
mkdir -p .task-build
sources=(src/pmp/surface_mesh.cpp src/pmp/algorithms/remeshing.cpp src/pmp/algorithms/normals.cpp src/pmp/algorithms/curvature.cpp src/pmp/algorithms/differential_geometry.cpp src/pmp/algorithms/laplace.cpp src/pmp/algorithms/distance_point_triangle.cpp src/pmp/algorithms/numerics.cpp)
for source in src/pmp/algorithms/*.cpp; do
 if ! grep -Fxq "$source" task_support/upstream-cpp.txt; then sources+=("$source"); fi
done
objects=()
for source in "${sources[@]}"; do objects+=(".task-build/${source%.cpp}.o"); done
make -s -f task_support/Makefile "${objects[@]}" .task-build/task_support/baseline.o
rm -f .task-build/libproduction.a
ar rcs .task-build/libproduction.a "${objects[@]}"
c++ .task-build/task_support/baseline.o .task-build/libproduction.a -o .task-build/baseline
