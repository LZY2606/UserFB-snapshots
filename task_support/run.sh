#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s tests/hierarchical_radiosity.cpp; fi
mkdir -p .task-build/objects
rm -f .task-build/objects/*.o .task-build/libengine.a
if [[ $mode == target ]]; then
  test -s examples/hierarchical_radiosity.cpp
  test -s raytracer/lighting/HierarchicalRadiosity.hpp
  test -s raytracer/lighting/HierarchicalRadiosity.cpp
  test -s raytracer/materials/HierarchicalDiffuse.hpp
  test -s raytracer/materials/HierarchicalDiffuse.cpp
  ${CXX:-c++} -std=c++17 -I. -include raytracer/lighting/HierarchicalRadiosity.hpp -include raytracer/materials/HierarchicalDiffuse.hpp -x c++ -c task_support/header_probe.inc -o .task-build/header_probe.o
fi
find raytracer -type f -name '*.cpp' ! -path '*/build/*' ! -path '*/image/*' ! -name raytracer.cpp | LC_ALL=C sort > .task-build/sources.list
test -s .task-build/sources.list
while IFS= read -r production_source; do
  object_name=${production_source//\//_}.o
  ${CXX:-c++} -std=c++17 -O1 -I. -c "$production_source" -o ".task-build/objects/$object_name"
done < .task-build/sources.list
ar rcs .task-build/libengine.a .task-build/objects/*.o
${CXX:-c++} -std=c++17 -O1 -I. task_support/baseline.cpp .task-build/libengine.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s examples/hierarchical_radiosity.cpp
${CXX:-c++} -std=c++17 -O1 -I. -include raytracer/lighting/HierarchicalRadiosity.hpp -include raytracer/materials/HierarchicalDiffuse.hpp -x c++ task_support/driver.c tests/hierarchical_radiosity.cpp -x none .task-build/objects/raytracer_lighting_HierarchicalRadiosity.cpp.o .task-build/objects/raytracer_materials_HierarchicalDiffuse.cpp.o .task-build/libengine.a -o .task-build/target
${CXX:-c++} -std=c++17 -O1 -I. -include raytracer/lighting/HierarchicalRadiosity.hpp -include raytracer/materials/HierarchicalDiffuse.hpp examples/hierarchical_radiosity.cpp .task-build/objects/raytracer_lighting_HierarchicalRadiosity.cpp.o .task-build/objects/raytracer_materials_HierarchicalDiffuse.cpp.o .task-build/libengine.a -o .task-build/example
example_dir=$(mktemp -d)
trap 'rm -rf "$example_dir"' EXIT
.task-build/example "$example_dir/radiosity.ppm"
test -s "$example_dir/radiosity.ppm"
head -n 1 "$example_dir/radiosity.ppm" | grep -qx P3
rm -rf "$example_dir"
trap - EXIT
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in links transport render; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
if [[ $mode == baseline ]]; then .task-build/baseline; fi
