#!/bin/bash
set -euo pipefail
cd "$(dirname "$0")/.."
mode=${1:-baseline}
if [[ $mode == target ]]; then test -s task_tests/injective.cpp; fi
mkdir -p .task-build
cmake -S . -B .task-build/core -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=11
cmake --build .task-build/core --target nvmesh libthekla_atlas -j 2
c++ -std=c++11 -O1 -Isrc task_support/baseline.cpp .task-build/core/liblibthekla_atlas.a .task-build/core/src/nvmesh/libnvmesh.a .task-build/core/src/nvimage/libnvimage.a .task-build/core/src/nvmath/libnvmath.a .task-build/core/src/nvcore/libnvcore.a .task-build/core/extern/poshlib/libposh.a -o .task-build/baseline
if [[ $mode == target ]]; then
  test -s src/nvmesh/param/InjectiveCharts.h
test -s src/nvmesh/param/InjectiveCharts.cpp
test -s examples/injective.cpp
ar -t .task-build/core/src/nvmesh/libnvmesh.a | grep -q 'InjectiveCharts.cpp'
c++ -std=c++11 -I. -Isrc -Isrc/nvcore -Iextern/poshlib -Iextern/tinyobj -include src/nvmesh/param/InjectiveCharts.h -x c++ -fsyntax-only /dev/null
c++ -std=c++11 -O1 -I. -c task_support/driver.c -o .task-build/driver.o
c++ -std=c++11 -O1 -I. -Isrc -Isrc/nvcore -Iextern/poshlib -Iextern/tinyobj -include src/nvmesh/param/InjectiveCharts.h task_tests/injective.cpp .task-build/driver.o .task-build/core/liblibthekla_atlas.a .task-build/core/src/nvmesh/libnvmesh.a .task-build/core/src/nvimage/libnvimage.a .task-build/core/src/nvmath/libnvmath.a .task-build/core/src/nvcore/libnvcore.a .task-build/core/extern/poshlib/libposh.a -o .task-build/target
c++ -std=c++11 -O1 -I. -Isrc -Isrc/nvcore -Iextern/poshlib -Iextern/tinyobj -include src/nvmesh/param/InjectiveCharts.h examples/injective.cpp .task-build/core/liblibthekla_atlas.a .task-build/core/src/nvmesh/libnvmesh.a .task-build/core/src/nvimage/libnvimage.a .task-build/core/src/nvmath/libnvmath.a .task-build/core/src/nvcore/libnvcore.a .task-build/core/extern/poshlib/libposh.a -o .task-build/example
example_dir=$(mktemp -d .task-build/example.XXXXXX)
.task-build/example "$example_dir/injective.svg"
test -s "$example_dir/injective.svg"
grep -q '<svg' "$example_dir/injective.svg"
  output_log=$(mktemp)
  trap 'rm -f "$output_log"' EXIT
  set +e
  .task-build/target >"$output_log" 2>&1
  status=$?
  set -e
  cat "$output_log"
  [[ $status == 0 ]] || exit "$status"
  for group in overlaps recut output; do
    grep -Eq "^GROUP ${group} cases=[1-9][0-9]* checks=[1-9][0-9]* COMPLETE$" "$output_log"
  done
  grep -Eq '^TARGET_COMPLETE cases=[1-9][0-9]*$' "$output_log"
fi
.task-build/baseline
