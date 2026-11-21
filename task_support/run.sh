#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/tet_transport.cpp; fi
mkdir -p .task-build
sources=(src/Air.cpp src/BSP.cpp src/Boundaries.cpp src/Camera.cpp src/Checkerboard.cpp src/Color.cpp src/CrissCross.cpp src/FlatColor.cpp src/Glass.cpp src/Image.cpp src/Intersection.cpp src/Light.cpp src/Marble.cpp src/Material.cpp src/NormalMap.cpp src/Object.cpp src/PerlinNoise.cpp src/Ray.cpp src/RayTracer.cpp src/ShinyColor.cpp src/Sphere.cpp src/Triangle.cpp src/Turbulence.cpp src/Vector.cpp src/Wood.cpp)
extra=()
if [[ $mode == target ]]; then
  for file in src/TetVolume.hpp src/TetVolume.cpp src/TetTransport.hpp src/TetTransport.cpp examples/tet_headless.cpp; do test -s "$file"; done
  extra=(src/TetVolume.cpp src/TetTransport.cpp)
  c++ -std=c++17 -O2 -I. -Isrc -include src/TetVolume.hpp -include src/TetTransport.hpp "${sources[@]}" "${extra[@]}" task_support/surface_baseline.cpp -o .task-build/baseline
else
  c++ -std=c++17 -O2 -I. -Isrc "${sources[@]}" task_support/surface_baseline.cpp -o .task-build/baseline
fi
if [[ $mode == target ]]; then
  c++ -std=c++17 -O2 -I. -Isrc -include src/TetVolume.hpp -include src/TetTransport.hpp "${sources[@]}" "${extra[@]}" tests/tet_transport.cpp task_support/driver.c -o .task-build/target
c++ -std=c++17 -O2 -I. -Isrc -include src/TetVolume.hpp -include src/TetTransport.hpp "${sources[@]}" "${extra[@]}" examples/tet_headless.cpp -o .task-build/tet_example
rm -f .task-build/tet.tga
.task-build/tet_example .task-build/tet.tga
test -s .task-build/tet.tga
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in geometry updates render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
