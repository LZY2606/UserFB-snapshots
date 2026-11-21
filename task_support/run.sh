#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/beamfilm.cpp; fi
mkdir -p .task-build
new_objects=""
if [[ $mode == target || -e src/beamfilm.cpp ]]; then
  test -s src/beamfilm.h
  test -s src/beamfilm.cpp
  c++ -std=c++11 -O1 -Wno-c++11-narrowing -I. -Isrc -include src/beamfilm.h -c src/beamfilm.cpp -o .task-build/beamfilm.o
  new_objects=".task-build/beamfilm.o"
fi
c++ -std=c++11 -O1 -Wno-c++11-narrowing -Isrc -I. -c src/material.cpp -o .task-build/material.o
c++ -std=c++11 -O1 -Wno-c++11-narrowing -Isrc -I. -c src/objects.cpp -o .task-build/objects.o
c++ -std=c++11 -O1 -Wno-c++11-narrowing -Isrc -I. -c src/kdtree.cpp -o .task-build/kdtree.o
c++ -std=c++11 -O1 -Wno-c++11-narrowing -Isrc -I. -c src/texture.cpp -o .task-build/texture.o
c++ -std=c++11 -O1 -Wno-c++11-narrowing -Isrc -I. -c src/camera.cpp -o .task-build/camera.o
c++ -std=c++11 -O1 -Wno-c++11-narrowing -Isrc -I. -c src/scene.cpp -o .task-build/scene.o
c++ -std=c++11 -O1 -Wno-c++11-narrowing -Isrc -I. -c src/renderer.cpp -o .task-build/renderer.o
cc -O1 -c lib/rand48/rand48.c -o .task-build/rand48.o
c++ -std=c++11 -O1 -c lib/lodepng/lodepng.cpp -o .task-build/lodepng.o
c++ -std=c++11 -O1 -c lib/tiny_obj_loader/tiny_obj_loader.cc -o .task-build/tinyobj.o
c++ -std=c++11 -x c++ -I. -Itask_support -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -Isrc -I. task_support/baseline.cpp .task-build/material.o .task-build/objects.o .task-build/kdtree.o .task-build/texture.o .task-build/camera.o .task-build/scene.o .task-build/renderer.o .task-build/rand48.o .task-build/lodepng.o .task-build/tinyobj.o $new_objects -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/beamfilm.cpp
c++ -std=c++11 -O1 -I. -Isrc task_tests/beamfilm.cpp .task-build/driver.o .task-build/material.o .task-build/objects.o .task-build/kdtree.o .task-build/texture.o .task-build/camera.o .task-build/scene.o .task-build/renderer.o .task-build/rand48.o .task-build/lodepng.o .task-build/tinyobj.o $new_objects -o .task-build/target
c++ -std=c++11 -O1 -I. -Isrc examples/beamfilm.cpp .task-build/material.o .task-build/objects.o .task-build/kdtree.o .task-build/texture.o .task-build/camera.o .task-build/scene.o .task-build/renderer.o .task-build/rand48.o .task-build/lodepng.o .task-build/tinyobj.o $new_objects -o .task-build/example
mkdir -p .task-build/beamfilm-output
rm -f .task-build/beamfilm-output/image.ppm .task-build/beamfilm-output/channels.csv
.task-build/example .task-build/beamfilm-output
test -s .task-build/beamfilm-output/image.ppm
test -s .task-build/beamfilm-output/channels.csv
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in events transport film; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
